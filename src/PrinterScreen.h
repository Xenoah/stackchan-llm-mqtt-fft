#pragma once

#include <M5Unified.h>

#include "PrinterState.h"

// 本体画面のプリンタ詳細画面（顔をタップすると開く、16bit フルカラー）。
// 進捗リング・残り時間・完成予定・温度・AMS・最新の実況をまとめて表示する。
void drawPrinterScreen(M5Canvas& canvas, const PrinterState& state,
                       const String& lastComment, bool voice);

// Keep the shared canvas; transfer only changed 16-row strips to the LCD.
class PrinterScreenRenderer {
 public:
  void invalidate() { valid_ = false; }
  void draw(M5Canvas& canvas, const PrinterState& state,
            const String& lastComment, bool voice);
  uint32_t frames() const { return frames_; }
  uint32_t pixels() const { return pixels_; }

 private:
  bool valid_ = false;
  uint64_t stateKey_ = 0;
  uint32_t strips_[15] = {};
  uint32_t frames_ = 0;
  uint32_t pixels_ = 0;
};
