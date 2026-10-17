#include "SetupScreens.h"

#include <WiFi.h>

#include <algorithm>
#include <vector>

#include "SetupUi.h"
#include "TouchKeyboard.h"

using namespace setupui;

namespace {

constexpr uint32_t kWifiTestTimeoutMs = 15000;
constexpr int kCloseId = 900;

// ---------------------------------------------------------------------------
// 共通
// ---------------------------------------------------------------------------

// 選択肢のボタンを縦に並べた画面。押したボタンの番号、×なら -1 を返す。
int runChoice(M5Canvas& canvas, const char* title, const String& line,
              uint16_t color, const std::vector<const char*>& labels,
              const std::function<void()>& service) {
  std::vector<Button> buttons;
  const int16_t h = 36;
  const int16_t gap = 8;
  int16_t y = kScreenH - 8 - static_cast<int16_t>(labels.size()) * (h + gap) + gap;
  for (size_t i = 0; i < labels.size(); ++i) {
    buttons.push_back({20, y, kScreenW - 40, h, static_cast<int>(i)});
    y += h + gap;
  }
  const Button close = {kScreenW - 36, 3, 32, 24, kCloseId};
  buttons.push_back(close);

  TouchTracker touch;
  touch.begin();
  bool dirty = true;
  while (true) {
    if (service) service();
    bool changed = false;
    const int released = touch.update(buttons, changed);
    if (released == kCloseId) return -1;
    if (released >= 0) return released;
    if (dirty || changed) {
      canvas.fillScreen(kBg);
      drawButton(canvas, close, "×", kRed, touch.pressed() == kCloseId);
      canvas.setFont(&fonts::lgfxJapanGothicP_16);
      canvas.setTextDatum(top_center);
      canvas.setTextColor(color, kBg);
      canvas.drawString(title, kScreenW / 2, 36);
      canvas.setTextColor(kSub, kBg);
      canvas.drawString(fitText(canvas, line, kScreenW - 24), kScreenW / 2, 62);
      for (size_t i = 0; i < labels.size(); ++i) {
        drawButton(canvas, buttons[i], labels[i], i == 0 ? kGreen : kBlue,
                   touch.pressed() == static_cast<int>(i), i == 0);
      }
      canvas.pushSprite(0, 0);
      dirty = false;
    }
    delay(8);
  }
}

// ---------------------------------------------------------------------------
// Wi-Fi
// ---------------------------------------------------------------------------

struct Network {
  String ssid;
  int32_t rssi;
  bool secure;
};

constexpr int kRescanId = 901;
constexpr int kManualId = 902;
constexpr int kUpId = 903;
constexpr int kDownId = 904;
constexpr int kRowsPerPage = 5;
constexpr int16_t kListTop = 32;
constexpr int16_t kRowH = 34;

int signalBars(int32_t rssi) {
  if (rssi >= -55) return 4;
  if (rssi >= -67) return 3;
  if (rssi >= -75) return 2;
  return 1;
}

void drawSignal(M5Canvas& c, int16_t x, int16_t bottom, int bars,
                uint16_t color) {
  for (int i = 0; i < 4; ++i) {
    const int16_t h = 4 + i * 4;
    const uint16_t col = i < bars ? color : kLine;
    c.fillRect(x + i * 5, bottom - h, 3, h, col);
  }
}

void drawLock(M5Canvas& c, int16_t x, int16_t y, uint16_t color) {
  c.drawRoundRect(x + 2, y, 7, 9, 3, color);
  c.fillRoundRect(x, y + 5, 11, 8, 2, color);
}

// スキャン結果を SSID ごとにまとめ（いちばん強い電波を残す）、強い順に並べる
std::vector<Network> collectNetworks(int count) {
  std::vector<Network> nets;
  for (int i = 0; i < count; ++i) {
    const String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) continue;
    const int32_t rssi = WiFi.RSSI(i);
    auto it = std::find_if(nets.begin(), nets.end(),
                           [&](const Network& n) { return n.ssid == ssid; });
    if (it != nets.end()) {
      if (rssi > it->rssi) it->rssi = rssi;
      continue;
    }
    nets.push_back({ssid, rssi, WiFi.encryptionType(i) != WIFI_AUTH_OPEN});
  }
  std::sort(nets.begin(), nets.end(),
            [](const Network& a, const Network& b) { return a.rssi > b.rssi; });
  if (nets.size() > 30) nets.resize(30);
  return nets;
}

void drawWifiList(M5Canvas& c, const std::vector<Network>& nets, int page,
                  bool scanning, const String& savedSsid,
                  const std::vector<Button>& buttons, int pressed) {
  c.fillScreen(kBg);
  drawHeader(c, "Wi-Fi をえらぶ", String());

  auto find = [&](int id) -> const Button* {
    for (const Button& b : buttons) {
      if (b.id == id) return &b;
    }
    return nullptr;
  };
  if (const Button* b = find(kRescanId)) {
    drawButton(c, *b, scanning ? "さがし中" : "再スキャン", kBlue, pressed == kRescanId);
  }
  if (const Button* b = find(kCloseId)) drawButton(c, *b, "×", kRed, pressed == kCloseId);

  if (scanning) {
    c.setFont(&fonts::lgfxJapanGothicP_16);
    c.setTextDatum(middle_center);
    c.setTextColor(kSub, kBg);
    c.drawString("まわりの Wi-Fi をさがしています…", kScreenW / 2, 110);
  } else if (nets.empty()) {
    c.setFont(&fonts::lgfxJapanGothicP_16);
    c.setTextDatum(middle_center);
    c.setTextColor(kSub, kBg);
    c.drawString("見つかりませんでした", kScreenW / 2, 100);
    c.drawString("再スキャンか、SSID を手入力してね", kScreenW / 2, 124);
  }

  for (int i = 0; i < kRowsPerPage; ++i) {
    const int index = page * kRowsPerPage + i;
    if (scanning || index >= static_cast<int>(nets.size())) break;
    const Network& n = nets[index];
    const Button* b = find(i);
    if (b == nullptr) break;
    const bool isPressed = pressed == i;
    const uint16_t fill = isPressed ? kBlue : kCard;
    const uint16_t fg = isPressed ? kBg : kText;
    c.fillRoundRect(b->x, b->y, b->w, b->h - 3, 7, fill);
    drawSignal(c, b->x + 8, b->y + 24, signalBars(n.rssi), isPressed ? kBg : kGreen);
    const bool saved = n.ssid == savedSsid;
    c.setFont(&fonts::lgfxJapanGothicP_16);
    c.setTextDatum(middle_left);
    c.setTextColor(fg, fill);
    const int16_t textX = b->x + 34;
    const int16_t right = b->x + b->w - (saved ? 72 : 24);
    c.drawString(fitText(c, n.ssid, right - textX), textX, b->y + 16);
    if (saved) {
      c.setFont(&fonts::lgfxJapanGothicP_12);
      c.setTextColor(isPressed ? kBg : kGreen, fill);
      c.drawString("保存済み", b->x + b->w - 70, b->y + 16);
    }
    if (n.secure) drawLock(c, b->x + b->w - 18, b->y + 9, isPressed ? kBg : kSub);
  }

  if (const Button* b = find(kManualId)) {
    drawButton(c, *b, "SSID を手入力", kBlue, pressed == kManualId);
  }
  const int pages = std::max<int>(1, (nets.size() + kRowsPerPage - 1) / kRowsPerPage);
  if (const Button* b = find(kUpId)) drawButton(c, *b, "▲", kBlue, pressed == kUpId);
  if (const Button* b = find(kDownId)) drawButton(c, *b, "▼", kBlue, pressed == kDownId);
  if (!scanning && pages > 1) {
    c.setFont(&fonts::Font0);
    c.setTextDatum(bottom_right);
    c.setTextColor(kSub, kBg);
    c.drawString(String(page + 1) + "/" + String(pages), kScreenW - 6, kListTop - 1);
  }
  c.pushSprite(0, 0);
}

// つながるか確かめている間の画面（経過をバーで出す）
void drawConnecting(M5Canvas& c, const String& ssid, uint32_t elapsed) {
  c.fillScreen(kBg);
  c.setFont(&fonts::lgfxJapanGothicP_16);
  c.setTextDatum(middle_center);
  c.setTextColor(kText, kBg);
  c.drawString("つないでいます…", kScreenW / 2, 88);
  c.setTextColor(kSub, kBg);
  c.drawString(fitText(c, ssid, kScreenW - 40), kScreenW / 2, 116);
  const int16_t barW = kScreenW - 80;
  c.fillRoundRect(40, 146, barW, 8, 4, kCard);
  const int16_t done = static_cast<int16_t>(
      barW * std::min<uint32_t>(elapsed, kWifiTestTimeoutMs) / kWifiTestTimeoutMs);
  c.fillRoundRect(40, 146, std::max<int16_t>(done, 8), 8, 4, kBlue);
  c.pushSprite(0, 0);
}

enum class ConnectResult { Saved, Back };

// パスワードを聞いて実際につないでみる。つながったら保存する。
ConnectResult connectFlow(M5Canvas& canvas, ConfigPortal& portal,
                          const String& ssid, bool secure,
                          const std::function<void()>& service) {
  const AppConfig& config = portal.config();
  const bool savedPassword =
      ssid == config.wifiSsid && !config.wifiPassword.isEmpty();
  String password;
  while (true) {
    if (secure) {
      KeyboardOptions options;
      options.title = "パスワード";
      options.hint = savedPassword ? ssid + "（空で決定=保存済み）" : ssid;
      options.secret = true;
      options.maxLength = 63;
      if (!runTouchKeyboard(canvas, password, options, service)) {
        return ConnectResult::Back;
      }
    }
    const String usePassword =
        password.isEmpty() && savedPassword ? config.wifiPassword : password;

    const uint32_t startedAt = millis();
    uint32_t drawnAt = 0;
    drawConnecting(canvas, ssid, 0);
    const bool ok = portal.testWifi(ssid, usePassword, kWifiTestTimeoutMs, [&]() {
      if (service) service();
      if (millis() - drawnAt >= 250) {
        drawnAt = millis();
        drawConnecting(canvas, ssid, millis() - startedAt);
      }
    });
    if (ok) {
      portal.saveWifi(ssid, usePassword);
      drawSetupMessage(canvas, "つながりました！",
                       "保存して再起動します（IP: " + WiFi.localIP().toString() + "）",
                       kGreen);
      delay(1500);
      return ConnectResult::Saved;
    }

    std::vector<const char*> labels;
    if (secure) labels.push_back("パスワードを入れなおす");
    labels.push_back("このまま保存する");
    labels.push_back("やめる");
    const int choice = runChoice(
        canvas, "つながりませんでした",
        secure ? String("パスワードと電波を確かめてね") : String("電波を確かめてね"),
        kRed, labels, service);
    const char* picked = choice >= 0 ? labels[choice] : "やめる";
    if (strcmp(picked, "パスワードを入れなおす") == 0) continue;
    if (strcmp(picked, "このまま保存する") == 0) {
      portal.saveWifi(ssid, usePassword);
      drawSetupMessage(canvas, "保存しました", "再起動します…", kGreen);
      delay(1000);
      return ConnectResult::Saved;
    }
    return ConnectResult::Back;
  }
}

// ---------------------------------------------------------------------------
// プリンター
// ---------------------------------------------------------------------------

enum PrinterRow { kRowEnabled, kRowHost, kRowSerial, kRowCode, kRowAuto, kRowCount };
constexpr int kSaveId = 910;
constexpr int kCancelId = 911;

struct PrinterForm {
  bool enabled;
  String host;
  String serial;
  String newCode;
  bool hasSavedCode;
  bool autoMode;
  String error;
};

void drawToggle(M5Canvas& c, int16_t right, int16_t cy, bool on, bool inverted) {
  const int16_t w = 46, h = 22;
  const int16_t x = right - w;
  const uint16_t track = on ? kGreen : (inverted ? kBg : kLine);
  c.fillRoundRect(x, cy - h / 2, w, h, h / 2, track);
  const int16_t knobX = on ? x + w - h / 2 : x + h / 2;
  c.fillCircle(knobX, cy, h / 2 - 3, on ? kBg : kSub);
}

void drawPrinterForm(M5Canvas& c, const PrinterForm& f,
                     const std::vector<Button>& buttons, int pressed) {
  c.fillScreen(kBg);
  if (f.error.isEmpty()) {
    drawHeader(c, "プリンター（MQTT）", String());
  } else {
    // 保存できなかった理由を見出しの位置に出す
    c.setFont(&fonts::lgfxJapanGothicP_16);
    c.setTextDatum(middle_left);
    c.setTextColor(kRed, kBg);
    c.drawString(f.error, 10, 15);
  }
  for (const Button& b : buttons) {
    if (b.id == kCloseId) drawButton(c, b, "×", kRed, pressed == kCloseId);
  }

  for (const Button& b : buttons) {
    if (b.id < 0 || b.id >= kRowCount) continue;
    const bool isPressed = pressed == b.id;
    const uint16_t fill = isPressed ? kBlue : kCard;
    const uint16_t fg = isPressed ? kBg : kText;
    const uint16_t sub = isPressed ? kBg : kSub;
    c.fillRoundRect(b.x, b.y, b.w, b.h, 7, fill);
    c.setFont(&fonts::lgfxJapanGothicP_16);
    c.setTextDatum(middle_left);
    c.setTextColor(sub, fill);
    const int16_t cy = b.y + b.h / 2 + 1;
    const int16_t right = b.x + b.w - 10;
    const char* label = "";
    String value;
    bool toggle = false;
    bool on = false;
    switch (b.id) {
      case kRowEnabled: label = "監視して実況する"; toggle = true; on = f.enabled; break;
      case kRowHost:    label = "IP アドレス"; value = f.host.isEmpty() ? "未設定" : f.host; break;
      case kRowSerial:  label = "シリアル番号"; value = f.serial.isEmpty() ? "未設定" : f.serial; break;
      case kRowCode:
        label = "アクセスコード";
        value = !f.newCode.isEmpty() ? String("新しいコード（") + f.newCode.length() + "桁）"
                : f.hasSavedCode    ? String("設定済み")
                                    : String("未設定");
        break;
      case kRowAuto:    label = "印刷で自動 MQTT"; toggle = true; on = f.autoMode; break;
    }
    c.drawString(label, b.x + 10, cy);
    if (toggle) {
      drawToggle(c, right, cy - 1, on, isPressed);
    } else {
      const int16_t labelW = c.textWidth(label);
      c.setTextDatum(middle_right);
      c.setTextColor(value == "未設定" ? (isPressed ? kBg : kYellow) : fg, fill);
      c.drawString(fitText(c, value, right - (b.x + 10 + labelW + 12)), right, cy);
    }
  }
  for (const Button& b : buttons) {
    if (b.id == kCancelId) drawButton(c, b, "やめる", kBlue, pressed == kCancelId);
    if (b.id == kSaveId) {
      drawButton(c, b, "保存して再起動", kGreen, pressed == kSaveId, true);
    }
  }
  c.pushSprite(0, 0);
}

// IP アドレスの形（数字と点）なら正しく読めるか確かめる
bool validHost(const String& host) {
  if (host.isEmpty()) return false;
  for (size_t i = 0; i < host.length(); ++i) {
    const char ch = host[i];
    if (!(ch == '.' || (ch >= '0' && ch <= '9'))) return true;  // ホスト名はそのまま
  }
  IPAddress ip;
  return ip.fromString(host);
}

}  // namespace

void drawSetupMessage(M5Canvas& canvas, const char* title, const String& line,
                      uint16_t color) {
  canvas.fillScreen(kBg);
  canvas.setFont(&fonts::lgfxJapanGothicP_16);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(color, kBg);
  canvas.drawString(title, kScreenW / 2, 100);
  canvas.setTextColor(kSub, kBg);
  canvas.drawString(fitText(canvas, line, kScreenW - 20), kScreenW / 2, 130);
  canvas.pushSprite(0, 0);
}

bool runWifiSetup(M5Canvas& canvas, ConfigPortal& portal,
                  const std::function<void()>& service) {
  std::vector<Network> nets;
  int page = 0;
  bool scanning = false;

  auto startScan = [&]() {
    WiFi.scanDelete();
    // セットアップ AP 中（AP_STA）でも、STA 接続中でもそのまま非同期でスキャンできる
    if (WiFi.getMode() == WIFI_MODE_NULL || WiFi.getMode() == WIFI_AP) {
      WiFi.mode(WIFI_AP_STA);
    }
    scanning = WiFi.scanNetworks(true, false) == WIFI_SCAN_RUNNING;
    if (!scanning) nets.clear();
    page = 0;
  };
  startScan();

  std::vector<Button> buttons;
  auto rebuild = [&]() {
    buttons.clear();
    for (int i = 0; i < kRowsPerPage; ++i) {
      const int index = page * kRowsPerPage + i;
      if (scanning || index >= static_cast<int>(nets.size())) break;
      buttons.push_back({6, static_cast<int16_t>(kListTop + i * kRowH),
                         kScreenW - 12, kRowH, i});
    }
    buttons.push_back({kScreenW - 132, 3, 92, 24, kRescanId});
    buttons.push_back({kScreenW - 36, 3, 32, 24, kCloseId});
    buttons.push_back({6, 204, 178, 32, kManualId});
    buttons.push_back({190, 204, 60, 32, kUpId});
    buttons.push_back({254, 204, 60, 32, kDownId});
  };
  rebuild();

  TouchTracker touch;
  touch.begin();
  bool dirty = true;
  while (true) {
    if (service) service();
    if (scanning) {
      const int16_t result = WiFi.scanComplete();
      if (result != WIFI_SCAN_RUNNING) {
        nets = result > 0 ? collectNetworks(result) : std::vector<Network>();
        WiFi.scanDelete();
        scanning = false;
        rebuild();
        dirty = true;
      }
    }

    bool changed = false;
    const int released = touch.update(buttons, changed);
    dirty |= changed;
    const int pages = std::max<int>(1, (nets.size() + kRowsPerPage - 1) / kRowsPerPage);
    String pickedSsid;
    bool pickedSecure = true;
    switch (released) {
      case -1:
        break;
      case kCloseId:
        WiFi.scanDelete();
        return false;
      case kRescanId:
        if (!scanning) startScan();
        rebuild();
        dirty = true;
        break;
      case kUpId:
        if (page > 0) --page;
        rebuild();
        dirty = true;
        break;
      case kDownId:
        if (page + 1 < pages) ++page;
        rebuild();
        dirty = true;
        break;
      case kManualId: {
        KeyboardOptions options;
        options.title = "SSID（ネットワーク名）";
        options.maxLength = 32;
        String ssid;
        if (runTouchKeyboard(canvas, ssid, options, service)) {
          ssid.trim();
          pickedSsid = ssid;
        }
        dirty = true;
        touch.begin();
        break;
      }
      default:
        if (released >= 0 && released < kRowsPerPage) {
          const int index = page * kRowsPerPage + released;
          if (index < static_cast<int>(nets.size())) {
            pickedSsid = nets[index].ssid;
            pickedSecure = nets[index].secure;
          }
        }
        break;
    }

    if (!pickedSsid.isEmpty()) {
      if (connectFlow(canvas, portal, pickedSsid, pickedSecure, service) ==
          ConnectResult::Saved) {
        return true;
      }
      touch.begin();
      dirty = true;
    }

    if (dirty) {
      drawWifiList(canvas, nets, page, scanning, portal.config().wifiSsid,
                   buttons, touch.pressed());
      dirty = false;
    }
    delay(8);
  }
}

bool runPrinterSetup(M5Canvas& canvas, ConfigPortal& portal,
                     const std::function<void()>& service) {
  const AppConfig& config = portal.config();
  PrinterForm form;
  form.enabled = config.bambuEnabled || config.bambuHost.isEmpty();
  form.host = config.bambuHost;
  form.serial = config.bambuSerial;
  form.hasSavedCode = !config.bambuAccessCode.isEmpty();
  form.autoMode = config.autoPrinterMode;

  std::vector<Button> buttons;
  for (int i = 0; i < kRowCount; ++i) {
    buttons.push_back({6, static_cast<int16_t>(32 + i * 34), kScreenW - 12, 31, i});
  }
  buttons.push_back({kScreenW - 36, 3, 32, 24, kCloseId});
  buttons.push_back({6, 204, 100, 32, kCancelId});
  buttons.push_back({112, 204, kScreenW - 118, 32, kSaveId});

  TouchTracker touch;
  touch.begin();
  bool dirty = true;
  while (true) {
    if (service) service();
    bool changed = false;
    const int released = touch.update(buttons, changed);
    dirty |= changed;

    if (released == kCloseId || released == kCancelId) return false;
    if (released >= 0 && released < kRowCount) {
      form.error = "";
      KeyboardOptions options;
      switch (released) {
        case kRowEnabled:
          form.enabled = !form.enabled;
          break;
        case kRowAuto:
          form.autoMode = !form.autoMode;
          break;
        case kRowHost:
          options.title = "IP アドレス";
          options.hint = "プリンターの 設定→WLAN";
          options.layout = KeyboardLayout::Numeric;
          options.maxLength = 39;
          runTouchKeyboard(canvas, form.host, options, service);
          form.host.trim();
          touch.begin();
          break;
        case kRowSerial:
          options.title = "シリアル番号";
          options.hint = "設定→デバイス";
          options.startUpper = true;
          options.maxLength = 24;
          runTouchKeyboard(canvas, form.serial, options, service);
          form.serial.trim();
          form.serial.toUpperCase();
          touch.begin();
          break;
        case kRowCode:
          options.title = "アクセスコード";
          options.hint = "設定→WLAN の8桁";
          options.secret = true;
          options.maxLength = 16;
          runTouchKeyboard(canvas, form.newCode, options, service);
          form.newCode.trim();
          touch.begin();
          break;
      }
      dirty = true;
    }
    if (released == kSaveId) {
      if (form.enabled && !validHost(form.host)) {
        form.error = "IP アドレスを入れてね";
      } else if (form.enabled && form.serial.isEmpty()) {
        form.error = "シリアル番号を入れてね";
      } else if (form.enabled && form.newCode.isEmpty() && !form.hasSavedCode) {
        form.error = "アクセスコードを入れてね";
      } else {
        portal.saveBambu(form.enabled, form.host, form.serial, form.newCode,
                         form.autoMode);
        drawSetupMessage(canvas, "保存しました", "再起動してプリンターにつなぎます…",
                         kGreen);
        delay(1200);
        return true;
      }
      dirty = true;
    }

    if (dirty) {
      drawPrinterForm(canvas, form, buttons, touch.pressed());
      dirty = false;
    }
    delay(8);
  }
}

SettingsChoice runSettingsMenu(M5Canvas& canvas, ConfigPortal& portal,
                               const std::function<void()>& service) {
  struct Item {
    SettingsChoice choice;
    const char* title;
    uint16_t accent;
  };
  static const Item kItems[] = {
      {SettingsChoice::Wifi, "Wi-Fi", kGreen},
      {SettingsChoice::Printer, "プリンター（MQTT）", kYellow},
      {SettingsChoice::Info, "接続情報・スマホで設定", kBlue},
  };
  std::vector<Button> buttons;
  for (int i = 0; i < 3; ++i) {
    buttons.push_back({8, static_cast<int16_t>(36 + i * 66), kScreenW - 16, 58, i});
  }
  buttons.push_back({kScreenW - 36, 3, 32, 24, kCloseId});

  TouchTracker touch;
  touch.begin();
  bool dirty = true;
  while (true) {
    if (service) service();
    bool changed = false;
    const int released = touch.update(buttons, changed);
    if (released == kCloseId) return SettingsChoice::Close;
    if (released >= 0 && released < 3) return kItems[released].choice;
    if (dirty || changed) {
      const AppConfig& config = portal.config();
      canvas.fillScreen(kBg);
      drawHeader(canvas, "設定", String());
      drawButton(canvas, buttons[3], "×", kRed, touch.pressed() == kCloseId);
      for (int i = 0; i < 3; ++i) {
        const Button& b = buttons[i];
        const bool pressed = touch.pressed() == i;
        const uint16_t fill = pressed ? kItems[i].accent : kCard;
        canvas.fillRoundRect(b.x, b.y, b.w, b.h, 10, fill);
        canvas.fillRoundRect(b.x, b.y + 12, 4, b.h - 24, 2, pressed ? kBg : kItems[i].accent);
        canvas.setFont(&fonts::lgfxJapanGothicP_16);
        canvas.setTextDatum(top_left);
        canvas.setTextColor(pressed ? kBg : kText, fill);
        canvas.drawString(kItems[i].title, b.x + 14, b.y + 9);
        String note;
        switch (kItems[i].choice) {
          case SettingsChoice::Wifi:
            note = portal.isConnected() ? "接続中: " + WiFi.SSID()
                   : config.wifiSsid.isEmpty() ? String("未設定")
                                               : "つながっていない: " + config.wifiSsid;
            break;
          case SettingsChoice::Printer:
            note = config.bambuHost.isEmpty()
                       ? String("未設定")
                       : String(config.bambuEnabled ? "監視中: " : "監視OFF: ") + config.bambuHost;
            break;
          default:
            note = portal.isConnected()
                       ? "http://" + WiFi.localIP().toString() + "/settings"
                       : String("設定用ホットスポットを立てる");
            break;
        }
        canvas.setFont(&fonts::lgfxJapanGothicP_12);
        canvas.setTextColor(pressed ? kBg : kSub, fill);
        canvas.drawString(fitText(canvas, note, b.w - 28), b.x + 14, b.y + 34);
      }
      canvas.pushSprite(0, 0);
      dirty = false;
    }
    delay(8);
  }
}
