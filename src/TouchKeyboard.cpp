#include "TouchKeyboard.h"

#include <vector>

#include "SetupUi.h"

using namespace setupui;

namespace {

enum class Page : uint8_t { Letters, Symbols, Numeric };

enum class KeyAction : uint8_t {
  Char,
  Shift,
  ToSymbols,
  ToLetters,
  Backspace,
  Space,
  Done,
};

struct Key {
  Button rect;
  KeyAction action;
  char ch;
};

// 特別なボタンの id（キーは 0 からの添字）
constexpr int kCancelId = 1000;
constexpr int kRevealId = 1001;

constexpr int16_t kKeysTop = 64;       // キーボード部分の上端
constexpr int16_t kSide = 2;           // 左右の余白
constexpr uint32_t kRepeatDelayMs = 500;
constexpr uint32_t kRepeatIntervalMs = 90;
constexpr uint32_t kDoubleTapMs = 400;
constexpr uint32_t kPeekMs = 1000;     // 伏せ字で最後の1文字を見せる時間

struct RowItem {
  float units;
  KeyAction action;
  char ch;
};

// 1行ぶんのキーを並べる（合計 10 ユニット = 画面幅）
void addRow(std::vector<Key>& keys, int16_t y, int16_t h,
            const std::vector<RowItem>& items) {
  const float unit = (kScreenW - kSide * 2) / 10.0f;
  float x = kSide;
  for (const RowItem& item : items) {
    const int16_t left = static_cast<int16_t>(x + 0.5f);
    const int16_t right = static_cast<int16_t>(x + item.units * unit + 0.5f);
    Key key;
    key.rect = {left, y, static_cast<int16_t>(right - left), h,
                static_cast<int>(keys.size())};
    key.action = item.action;
    key.ch = item.ch;
    keys.push_back(key);
    x += item.units * unit;
  }
}

std::vector<RowItem> charRow(const char* chars) {
  std::vector<RowItem> row;
  for (const char* p = chars; *p; ++p) row.push_back({1.0f, KeyAction::Char, *p});
  return row;
}

void buildLayout(Page page, std::vector<Key>& keys) {
  keys.clear();
  if (page == Page::Numeric) {
    const int16_t h = (kScreenH - kKeysTop - 2) / 4;
    const char* const rows[] = {"123", "456", "7890"};
    for (int r = 0; r < 3; ++r) {
      std::vector<RowItem> row;
      for (const char* p = rows[r]; *p; ++p) row.push_back({2.5f, KeyAction::Char, *p});
      if (r == 0) row.push_back({2.5f, KeyAction::Backspace, 0});
      if (r == 1) row.push_back({2.5f, KeyAction::Char, '.'});
      addRow(keys, kKeysTop + r * h, h, row);
    }
    addRow(keys, kKeysTop + 3 * h, h, {{10.0f, KeyAction::Done, 0}});
    return;
  }

  const int16_t h = (kScreenH - kKeysTop - 2) / 5;
  if (page == Page::Letters) {
    addRow(keys, kKeysTop, h, charRow("1234567890"));
    addRow(keys, kKeysTop + h, h, charRow("qwertyuiop"));
    addRow(keys, kKeysTop + 2 * h, h, charRow("asdfghjkl-"));
    std::vector<RowItem> row = {{1.5f, KeyAction::Shift, 0}};
    for (const RowItem& item : charRow("zxcvbnm")) row.push_back(item);
    row.push_back({1.5f, KeyAction::Backspace, 0});
    addRow(keys, kKeysTop + 3 * h, h, row);
    addRow(keys, kKeysTop + 4 * h, h,
           {{1.5f, KeyAction::ToSymbols, 0},
            {1.0f, KeyAction::Char, '_'},
            {1.0f, KeyAction::Char, '@'},
            {2.5f, KeyAction::Space, ' '},
            {1.0f, KeyAction::Char, '.'},
            {3.0f, KeyAction::Done, 0}});
    return;
  }

  // 記号ページ（印字可能な ASCII をすべて打てる）
  addRow(keys, kKeysTop, h, charRow("1234567890"));
  addRow(keys, kKeysTop + h, h, charRow("!@#$%^&*()"));
  addRow(keys, kKeysTop + 2 * h, h, charRow("-_=+[]{};:"));
  std::vector<RowItem> row = {{1.5f, KeyAction::ToLetters, 0}};
  for (const RowItem& item : charRow("'\",./?\\")) row.push_back(item);
  row.push_back({1.5f, KeyAction::Backspace, 0});
  addRow(keys, kKeysTop + 3 * h, h, row);
  std::vector<RowItem> last = charRow("<>|~`");
  last.push_back({2.0f, KeyAction::Space, ' '});
  last.push_back({3.0f, KeyAction::Done, 0});
  addRow(keys, kKeysTop + 4 * h, h, last);
}

// UTF-8 の最後の1文字を消す
void eraseLastChar(String& text) {
  if (text.isEmpty()) return;
  int i = text.length() - 1;
  while (i > 0 && (static_cast<uint8_t>(text[i]) & 0xC0) == 0x80) --i;
  text.remove(i);
}

bool isAscii(const String& text) {
  for (size_t i = 0; i < text.length(); ++i) {
    if (static_cast<uint8_t>(text[i]) >= 0x80) return false;
  }
  return true;
}

size_t charCount(const String& text) {
  size_t n = 0;
  for (size_t i = 0; i < text.length(); ++i) {
    if ((static_cast<uint8_t>(text[i]) & 0xC0) != 0x80) ++n;
  }
  return n;
}

// シフトキーの上向き矢印
void drawShiftIcon(M5Canvas& c, int16_t cx, int16_t cy, uint16_t color,
                   bool filled) {
  const int16_t top = cy - 8;
  if (filled) {
    c.fillTriangle(cx, top, cx - 8, cy, cx + 8, cy, color);
    c.fillRect(cx - 4, cy, 9, 7, color);
  } else {
    c.drawTriangle(cx, top, cx - 8, cy, cx + 8, cy, color);
    c.drawRect(cx - 4, cy, 9, 7, color);
  }
}

// 後退キーの左向き矢印（×入り）
void drawBackspaceIcon(M5Canvas& c, int16_t cx, int16_t cy, uint16_t color) {
  c.drawLine(cx - 10, cy, cx - 4, cy - 7, color);
  c.drawLine(cx - 10, cy, cx - 4, cy + 7, color);
  c.drawLine(cx - 4, cy - 7, cx + 10, cy - 7, color);
  c.drawLine(cx - 4, cy + 7, cx + 10, cy + 7, color);
  c.drawLine(cx + 10, cy - 7, cx + 10, cy + 7, color);
  c.drawLine(cx - 1, cy - 3, cx + 5, cy + 3, color);
  c.drawLine(cx - 1, cy + 3, cx + 5, cy - 3, color);
}

struct State {
  String text;
  Page page = Page::Letters;
  bool shift = false;
  bool capsLock = false;
  bool reveal = false;
  uint32_t lastShiftAt = 0;
  uint32_t lastTypedAt = 0;
};

void drawKeyboard(M5Canvas& c, const KeyboardOptions& opt, const State& st,
                  const std::vector<Key>& keys, int pressed,
                  const Button& cancel, const Button& reveal) {
  c.fillScreen(kBg);

  // 見出しと×・表示切り替え
  drawButton(c, cancel, "×", kRed, pressed == kCancelId);
  c.setFont(&fonts::lgfxJapanGothicP_16);
  c.setTextDatum(middle_left);
  c.setTextColor(kText, kBg);
  const int16_t titleX = cancel.x + cancel.w + 8;
  c.drawString(opt.title, titleX, 15);
  if (!opt.hint.isEmpty()) {
    c.setFont(&fonts::lgfxJapanGothicP_12);
    c.setTextColor(kSub, kBg);
    const int16_t hintX =
        titleX + c.textWidth(opt.title, &fonts::lgfxJapanGothicP_16) + 8;
    const int16_t right = opt.secret ? reveal.x - 6 : kScreenW - 8;
    c.drawString(fitText(c, opt.hint, right - hintX), hintX, 16);
  }
  if (opt.secret) {
    drawButton(c, reveal, st.reveal ? "隠す" : "表示", kBlue, pressed == kRevealId);
  }

  // 入力欄
  const int16_t boxX = 6, boxY = 30, boxW = kScreenW - 12, boxH = 30;
  c.fillRoundRect(boxX, boxY, boxW, boxH, 7, rgb565(17, 20, 26));
  c.drawRoundRect(boxX, boxY, boxW, boxH, 7, kBlue);
  String shown = st.text;
  if (opt.secret && !st.reveal) {
    // 最後に打った1文字だけ少し見せる
    const size_t n = charCount(st.text);
    const bool peek = n > 0 && millis() - st.lastTypedAt < kPeekMs;
    shown = "";
    for (size_t i = 0; i + (peek ? 1 : 0) < n; ++i) shown += "*";
    if (peek) {
      int i = st.text.length() - 1;
      while (i > 0 && (static_cast<uint8_t>(st.text[i]) & 0xC0) == 0x80) --i;
      shown += st.text.substring(i);
    }
  }
  const lgfx::IFont* font = isAscii(shown)
                                ? static_cast<const lgfx::IFont*>(&fonts::AsciiFont8x16)
                                : &fonts::lgfxJapanGothicP_16;
  c.setFont(font);
  c.setTextDatum(middle_left);
  c.setTextColor(kText);
  const int16_t maxTextW = boxW - 24;
  // 長いときは末尾が見えるように先頭を省く
  String visible = shown;
  while (!visible.isEmpty() && c.textWidth(visible) > maxTextW) {
    int cut = 1;
    while (cut < static_cast<int>(visible.length()) &&
           (static_cast<uint8_t>(visible[cut]) & 0xC0) == 0x80) {
      ++cut;
    }
    visible.remove(0, cut);
  }
  const int16_t textX = boxX + 10;
  c.drawString(visible, textX, boxY + boxH / 2 + 1);
  const int16_t caretX = textX + c.textWidth(visible) + 1;
  c.fillRect(caretX, boxY + 7, 2, boxH - 14, kBlue);
  // 文字数（長さ制限の目安）
  c.setFont(&fonts::Font0);
  c.setTextDatum(bottom_right);
  c.setTextColor(kSub);
  c.drawString(String(charCount(st.text)), boxX + boxW - 5, boxY + boxH - 2);

  // キー
  const bool upper = st.shift || st.capsLock;
  for (const Key& key : keys) {
    const Button& r = key.rect;
    const bool isPressed = pressed == r.id;
    uint16_t fill = kCard;
    uint16_t fg = kText;
    if (key.action == KeyAction::Done) {
      fill = isPressed ? kText : kGreen;
      fg = kBg;
    } else if (key.action != KeyAction::Char && key.action != KeyAction::Space) {
      fill = kCardHi;
    }
    if (key.action == KeyAction::Shift && st.capsLock) fill = kBlue;
    if (isPressed && key.action != KeyAction::Done) {
      fill = kBlue;
      fg = kBg;
    }
    c.fillRoundRect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, 5, fill);
    const int16_t cx = r.x + r.w / 2;
    const int16_t cy = r.y + r.h / 2;
    c.setTextColor(fg, fill);
    c.setTextDatum(middle_center);
    switch (key.action) {
      case KeyAction::Char: {
        char label[2] = {key.ch, 0};
        if (upper && key.ch >= 'a' && key.ch <= 'z') label[0] = key.ch - 'a' + 'A';
        c.setFont(&fonts::FreeSansBold9pt7b);
        c.drawString(label, cx, cy);
        break;
      }
      case KeyAction::Shift:
        drawShiftIcon(c, cx, cy + 1, st.capsLock || isPressed ? kBg : fg,
                      upper);
        break;
      case KeyAction::Backspace:
        drawBackspaceIcon(c, cx, cy, fg);
        break;
      case KeyAction::ToSymbols:
        c.setFont(&fonts::FreeSansBold9pt7b);
        c.drawString("#+=", cx, cy);
        break;
      case KeyAction::ToLetters:
        c.setFont(&fonts::FreeSansBold9pt7b);
        c.drawString("abc", cx, cy);
        break;
      case KeyAction::Space:
        c.setFont(&fonts::lgfxJapanGothicP_16);
        c.drawString("空白", cx, cy + 1);
        break;
      case KeyAction::Done:
        c.setFont(&fonts::lgfxJapanGothicP_16);
        c.drawString("決定", cx, cy + 1);
        break;
    }
  }
  c.pushSprite(0, 0);
}

}  // namespace

bool runTouchKeyboard(M5Canvas& canvas, String& value,
                      const KeyboardOptions& options,
                      const std::function<void()>& service) {
  State st;
  st.text = value;
  st.page = options.layout == KeyboardLayout::Numeric ? Page::Numeric
                                                       : Page::Letters;
  st.capsLock = options.startUpper;
  st.reveal = !options.secret;

  std::vector<Key> keys;
  buildLayout(st.page, keys);
  const Button cancel = {4, 3, 30, 24, kCancelId};
  const Button reveal = {kScreenW - 58, 3, 54, 24, kRevealId};

  std::vector<Button> buttons;
  auto rebuildButtons = [&]() {
    buttons.clear();
    for (const Key& key : keys) buttons.push_back(key.rect);
    buttons.push_back(cancel);
    if (options.secret) buttons.push_back(reveal);
  };
  rebuildButtons();

  TouchTracker touch;
  touch.begin();
  bool dirty = true;
  bool peekShown = false;
  uint32_t lastRepeatAt = 0;
  bool repeated = false;

  auto type = [&](char ch) {
    if (charCount(st.text) >= options.maxLength) return;
    char c = ch;
    if ((st.shift || st.capsLock) && c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    st.text += c;
    st.lastTypedAt = millis();
    peekShown = true;
    if (st.shift && !st.capsLock) st.shift = false;
  };

  while (true) {
    if (service) service();
    bool changed = false;
    const int released = touch.update(buttons, changed);
    dirty |= changed;

    // 後退キーを押し続けたら連続で消す
    const int held = touch.pressed();
    if (held >= 0 && held < static_cast<int>(keys.size()) &&
        keys[held].action == KeyAction::Backspace &&
        touch.pressedFor() >= kRepeatDelayMs &&
        millis() - lastRepeatAt >= kRepeatIntervalMs) {
      eraseLastChar(st.text);
      lastRepeatAt = millis();
      repeated = true;
      dirty = true;
    }

    if (released == kCancelId) return false;
    if (released == kRevealId) {
      st.reveal = !st.reveal;
      dirty = true;
    } else if (released >= 0 && released < static_cast<int>(keys.size())) {
      const Key key = keys[released];
      switch (key.action) {
        case KeyAction::Char:
        case KeyAction::Space:
          type(key.ch);
          break;
        case KeyAction::Backspace:
          if (!repeated) eraseLastChar(st.text);
          break;
        case KeyAction::Shift: {
          const uint32_t now = millis();
          if (st.capsLock) {
            st.capsLock = false;
            st.shift = false;
          } else if (st.shift && now - st.lastShiftAt < kDoubleTapMs) {
            st.capsLock = true;  // 2回続けて押すと大文字固定
          } else {
            st.shift = !st.shift;
          }
          st.lastShiftAt = now;
          break;
        }
        case KeyAction::ToSymbols:
        case KeyAction::ToLetters:
          st.page = key.action == KeyAction::ToSymbols ? Page::Symbols
                                                       : Page::Letters;
          buildLayout(st.page, keys);
          rebuildButtons();
          break;
        case KeyAction::Done:
          value = st.text;
          return true;
      }
      dirty = true;
    }
    if (released != -1 || held == -1) repeated = false;

    // 伏せ字で見せていた最後の1文字を隠す
    if (peekShown && millis() - st.lastTypedAt >= kPeekMs) {
      peekShown = false;
      if (options.secret && !st.reveal) dirty = true;
    }

    if (dirty) {
      drawKeyboard(canvas, options, st, keys, touch.pressed(), cancel, reveal);
      dirty = false;
    }
    delay(8);
  }
}
