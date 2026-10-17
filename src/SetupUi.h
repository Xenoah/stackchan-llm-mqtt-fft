#pragma once

#include <M5Unified.h>

#include <vector>

// 本体の設定画面（キーボード・Wi-Fi・プリンター）で共有する見た目と操作の部品。
// 色はメニュー・設定情報画面と揃えている。
namespace setupui {

constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

constexpr uint16_t kBg = rgb565(14, 16, 20);
// Keep these levels distinct after RGB332 quantization on the 8bit canvas.
constexpr uint16_t kCard = rgb565(32, 32, 64);
constexpr uint16_t kCardHi = rgb565(64, 64, 128);
constexpr uint16_t kLine = rgb565(64, 64, 64);
constexpr uint16_t kText = rgb565(233, 235, 241);
constexpr uint16_t kSub = rgb565(139, 146, 163);
constexpr uint16_t kGreen = rgb565(74, 222, 128);
constexpr uint16_t kBlue = rgb565(96, 165, 250);
constexpr uint16_t kYellow = rgb565(250, 204, 21);
constexpr uint16_t kRed = rgb565(248, 113, 113);

constexpr int16_t kScreenW = 320;
constexpr int16_t kScreenH = 240;

struct Button {
  int16_t x, y, w, h;
  int id;
  bool contains(int16_t px, int16_t py) const {
    return px >= x && px < x + w && py >= y && py < y + h;
  }
};

// 押している間はそのボタンを押下表示にし、指を離したときに押していたボタンを確定する。
// 指をずらすと押下表示も移る（メニューと同じ操作感）。
class TouchTracker {
 public:
  // 画面に触れたまま始まったら、いったん離すまで入力を受け付けない
  void begin() {
    int16_t x, y;
    ignoreUntilRelease_ = M5.Display.getTouch(&x, &y);
    pressed_ = -1;
    wasTouching_ = false;
  }

  // 戻り値: 離したボタンの id（なければ -1）。changed は押下表示が変わったら true
  int update(const std::vector<Button>& buttons, bool& changed) {
    int16_t x = 0, y = 0;
    const bool touching = M5.Display.getTouch(&x, &y);
    int released = -1;
    if (ignoreUntilRelease_) {
      if (!touching) ignoreUntilRelease_ = false;
      wasTouching_ = touching;
      return -1;
    }
    if (touching) {
      int hit = -1;
      for (const Button& b : buttons) {
        if (b.contains(x, y)) {
          hit = b.id;
          break;
        }
      }
      if (!wasTouching_) pressedAt_ = millis();
      if (hit != pressed_) {
        pressed_ = hit;
        pressedAt_ = millis();
        changed = true;
      }
    } else if (wasTouching_) {
      released = pressed_;
      if (pressed_ != -1) changed = true;
      pressed_ = -1;
    }
    wasTouching_ = touching;
    return released;
  }

  int pressed() const { return pressed_; }
  uint32_t pressedFor() const { return pressed_ == -1 ? 0 : millis() - pressedAt_; }

 private:
  bool ignoreUntilRelease_ = false;
  bool wasTouching_ = false;
  int pressed_ = -1;
  uint32_t pressedAt_ = 0;
};

// 角丸のボタンを描く（押下中は accent で塗る）
inline void drawButton(M5Canvas& c, const Button& b, const char* label,
                       uint16_t accent, bool pressed, bool filled = false) {
  const uint16_t fill = pressed ? accent : (filled ? accent : kCard);
  const uint16_t text = (pressed || filled) ? kBg : kText;
  c.fillRoundRect(b.x, b.y, b.w, b.h, 8, fill);
  if (!pressed && !filled) c.drawRoundRect(b.x, b.y, b.w, b.h, 8, kLine);
  c.setFont(&fonts::lgfxJapanGothicP_16);
  c.setTextDatum(middle_center);
  c.setTextColor(text, fill);
  c.drawString(label, b.x + b.w / 2, b.y + b.h / 2 + 1);
}

// 見出し（左上）と、右上の「×」ボタン（id を返す位置は呼び出し側で登録する）
inline void drawHeader(M5Canvas& c, const char* title, const String& note) {
  c.setFont(&fonts::lgfxJapanGothicP_16);
  c.setTextDatum(middle_left);
  c.setTextColor(kText, kBg);
  c.drawString(title, 10, 15);
  if (!note.isEmpty()) {
    c.setFont(&fonts::lgfxJapanGothicP_12);
    c.setTextColor(kSub, kBg);
    c.drawString(note, 16 + c.textWidth(title, &fonts::lgfxJapanGothicP_16), 16);
  }
}

// 幅に収まるよう末尾を「…」で切った文字列（UTF-8 の文字単位で切る）
inline String fitText(M5Canvas& c, const String& text, int maxWidth) {
  if (c.textWidth(text) <= maxWidth) return text;
  String s = text;
  while (!s.isEmpty()) {
    // 末尾の1文字（UTF-8）を落とす
    int i = s.length() - 1;
    while (i > 0 && (static_cast<uint8_t>(s[i]) & 0xC0) == 0x80) --i;
    s.remove(i);
    if (c.textWidth(s + "…") <= maxWidth) return s + "…";
  }
  return String("…");
}

}  // namespace setupui
