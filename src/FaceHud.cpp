#include "FaceHud.h"

#include <M5Unified.h>

namespace {

const lgfx::IFont* const kJpFont = &fonts::lgfxJapanGothicP_16;
const lgfx::IFont* const kBigFont = &fonts::Font4;

constexpr int kWidth = 320;
constexpr int kHeight = 240;
constexpr int kBottomTop = 202;        // 下段（字幕・情報）の開始 Y
constexpr uint32_t kCaptionPageMs = 3200;  // 字幕が3行以上のときのページ送り間隔

// UTF-8 の先頭バイトから1文字のバイト数を求める
size_t utf8Length(uint8_t lead) {
  if (lead >= 0xF0) return 4;
  if (lead >= 0xE0) return 3;
  if (lead >= 0xC0) return 2;
  return 1;
}

// 行頭に来ると読みにくい約物（ぶら下げて前の行に残す）
bool isClosingPunctuation(const char* glyph) {
  static const char* const kMarks[] = {"、", "。", "！", "？", "」", "）",
                                       "…", "ー", ",", ".", "!", "?"};
  for (const char* mark : kMarks) {
    if (strcmp(glyph, mark) == 0) return true;
  }
  return false;
}

}  // namespace

FaceHud::FaceHud() {
  mux_ = portMUX_INITIALIZER_UNLOCKED;
}

void FaceHud::set(const HudData& data) {
  portENTER_CRITICAL(&mux_);
  data_ = data;
  portEXIT_CRITICAL(&mux_);
}

void FaceHud::setSpectrum(bool visible, const uint8_t (&bars)[32]) {
  portENTER_CRITICAL(&mux_);
  spectrumVisible_ = visible;
  memcpy(spectrumBars_, bars, sizeof(spectrumBars_));
  portEXIT_CRITICAL(&mux_);
}

bool FaceHud::hasSpectrum() {
  portENTER_CRITICAL(&mux_);
  const bool visible = spectrumVisible_;
  portEXIT_CRITICAL(&mux_);
  return visible;
}

void FaceHud::setCaption(const String& text, uint32_t durationMs) {
  const uint32_t now = millis();
  portENTER_CRITICAL(&mux_);
  strncpy(caption_, text.c_str(), sizeof(caption_) - 1);
  caption_[sizeof(caption_) - 1] = '\0';
  // 途中で切れた UTF-8 の断片を取り除く
  size_t len = strlen(caption_);
  size_t i = len;
  while (i > 0 && (static_cast<uint8_t>(caption_[i - 1]) & 0xC0) == 0x80) --i;
  if (i > 0 && utf8Length(static_cast<uint8_t>(caption_[i - 1])) > len - i + 1) {
    caption_[i - 1] = '\0';
  }
  captionSince_ = now;
  captionUntil_ = durationMs == 0 ? 0 : now + durationMs;
  portEXIT_CRITICAL(&mux_);
}

void FaceHud::clearCaption() {
  portENTER_CRITICAL(&mux_);
  caption_[0] = '\0';
  captionUntil_ = 0;
  portEXIT_CRITICAL(&mux_);
}

bool FaceHud::hasCaption() {
  const uint32_t now = millis();
  portENTER_CRITICAL(&mux_);
  const bool live = caption_[0] != '\0' &&
      (captionUntil_ == 0 || static_cast<int32_t>(now - captionUntil_) < 0);
  portEXIT_CRITICAL(&mux_);
  return live;
}

void FaceHud::setToast(const char* text, uint32_t durationMs) {
  const uint32_t now = millis();
  portENTER_CRITICAL(&mux_);
  strncpy(toast_, text ? text : "", sizeof(toast_) - 1);
  toast_[sizeof(toast_) - 1] = '\0';
  toastUntil_ = durationMs == 0 ? 0 : now + durationMs;
  portEXIT_CRITICAL(&mux_);
}

void FaceHud::draw(M5Canvas* canvas, int colorDepth, uint16_t fg,
                   uint16_t bg) {
  (void)colorDepth;
  HudData d;
  uint8_t spectrumBars[32];
  bool spectrumVisible;
  char caption[sizeof(caption_)];
  char toast[sizeof(toast_)];
  uint32_t since = 0;
  const uint32_t now = millis();

  portENTER_CRITICAL(&mux_);
  d = data_;
  spectrumVisible = spectrumVisible_;
  memcpy(spectrumBars, spectrumBars_, sizeof(spectrumBars));
  const bool captionLive =
      caption_[0] != '\0' &&
      (captionUntil_ == 0 || static_cast<int32_t>(now - captionUntil_) < 0);
  if (captionLive) {
    memcpy(caption, caption_, sizeof(caption));
  } else {
    caption[0] = '\0';
  }
  since = captionSince_;
  const bool toastLive =
      toast_[0] != '\0' &&
      (toastUntil_ == 0 || static_cast<int32_t>(now - toastUntil_) < 0);
  if (toastLive) {
    memcpy(toast, toast_, sizeof(toast));
  } else {
    toast[0] = '\0';
  }
  portEXIT_CRITICAL(&mux_);

  if (!d.visible && !spectrumVisible && caption[0] == '\0') return;

  if (spectrumVisible) {
    // Draw within the face sprite: no competing LCD transfers. Captions keep
    // their separate area below y=202, including while TTS ducks USB audio.
    canvas->fillRect(0, 164, kWidth, 38, bg);
    canvas->setFont(&fonts::Font0);
    canvas->setTextSize(1);
    canvas->setTextDatum(top_left);
    canvas->setTextColor(fg, bg);
    canvas->drawString("USB FFT", 8, 165);
    canvas->drawFastHLine(8, 199, 304, fg);
    for (int i = 0; i < 32; ++i) {
      const int height = spectrumBars[i] * 25 / 100;
      if (height) canvas->fillRect(8 + i * 304 / 32, 198 - height, 7, height, fg);
    }
  }

  canvas->setTextSize(1);
  canvas->setTextWrap(false);
  if (d.visible) drawTopBar(canvas, d, fg, bg, now);
  if (d.visible || caption[0] || toast[0]) {
    drawBottom(canvas, d, caption, since, toast, fg, bg, now);
  }
}

void FaceHud::drawTopBar(M5Canvas* c, const HudData& d, uint16_t fg,
                         uint16_t bg, uint32_t now) {
  constexpr int chipX = 4;
  constexpr int chipY = 3;
  constexpr int chipH = 21;

  c->setFont(kJpFont);
  const int chipW = c->textWidth(d.phase) + 16;
  const bool blinkOff = d.alert && ((now / 450) % 2 == 1);
  if (blinkOff) {
    c->fillRoundRect(chipX, chipY, chipW, chipH, 7, bg);
    c->drawRoundRect(chipX, chipY, chipW, chipH, 7, fg);
    c->setTextColor(fg, bg);
  } else {
    c->fillRoundRect(chipX, chipY, chipW, chipH, 7, fg);
    c->setTextColor(bg, fg);
  }
  c->setTextDatum(middle_center);
  c->drawString(d.phase, chipX + chipW / 2, chipY + chipH / 2 + 1);

  if (!d.active || d.percent < 0) return;

  // 進捗 %（右上に大きく）
  char pct[8];
  snprintf(pct, sizeof(pct), "%d%%", d.percent);
  c->setFont(kBigFont);
  c->setTextColor(fg, bg);
  c->setTextDatum(top_right);
  c->fillRect(kWidth - 80, 0, 80, 28, bg);
  c->drawString(pct, kWidth - 4, 2);
  const int pctW = c->textWidth(pct);

  // 残り時間（% の左）
  if (d.remaining[0] != '\0') {
    char rem[24];
    snprintf(rem, sizeof(rem), "残り %s", d.remaining);
    c->setFont(kJpFont);
    c->setTextDatum(middle_right);
    c->drawString(rem, kWidth - 4 - pctW - 10, chipY + chipH / 2 + 1);
  }

  // 進捗バー
  constexpr int bx = 4;
  constexpr int by = 29;
  constexpr int bw = kWidth - 8;
  constexpr int bh = 8;
  c->fillRect(bx, by, bw, bh, bg);
  c->drawRoundRect(bx, by, bw, bh, 3, fg);
  const int fillW = (bw - 4) * constrain(d.percent, 0, 100) / 100;
  if (fillW > 0) {
    c->fillRect(bx + 2, by + 2, fillW, bh - 4, fg);
  }
}

void FaceHud::drawBottom(M5Canvas* c, const HudData& d, const char* caption,
                         uint32_t captionSince, const char* toast,
                         uint16_t fg, uint16_t bg, uint32_t now) {
  c->setFont(kJpFont);

  // 実況字幕（最優先）
  if (caption[0] != '\0') {
    c->fillRect(0, kBottomTop, kWidth, kHeight - kBottomTop, bg);
    c->drawFastHLine(8, kBottomTop, kWidth - 16, fg);
    if (captionSince != cachedSince_ || strcmp(caption, cachedCaption_) != 0) {
      cachedSince_ = captionSince;
      cachedLines_ = drawWrapped(c, caption, 8, 0, kWidth - 16, 18, 0, 0, fg, bg);
    }
    int page = 0;
    const int pages = (cachedLines_ + 1) / 2;
    if (pages > 1) {
      page = static_cast<int>(((now - captionSince) / kCaptionPageMs) % pages);
    }
    c->setTextColor(fg, bg);
    drawWrapped(c, caption, 8, kBottomTop + 4, kWidth - 16, 18, page * 2, 2,
                fg, bg);
    return;
  }

  // 短いステータス（"TTS" など）
  if (toast[0] != '\0') {
    const int w = c->textWidth(toast) + 24;
    const int x = (kWidth - w) / 2;
    c->fillRoundRect(x, kBottomTop + 10, w, 24, 8, fg);
    c->setTextColor(bg, fg);
    c->setTextDatum(middle_center);
    c->drawString(toast, kWidth / 2, kBottomTop + 23);
    return;
  }

  // 温度・層・ジョブ名
  char temps[48] = "";
  if (d.nozzle != INT16_MIN || d.bed != INT16_MIN) {
    char n[8] = "-";
    char b[8] = "-";
    if (d.nozzle != INT16_MIN) snprintf(n, sizeof(n), "%d", d.nozzle);
    if (d.bed != INT16_MIN) snprintf(b, sizeof(b), "%d", d.bed);
    snprintf(temps, sizeof(temps), "ノズル %s℃  ベッド %s℃", n, b);
  }

  c->setTextColor(fg, bg);
  const int line1 = kBottomTop + 13;
  const int line2 = kBottomTop + 30;
  if (d.active) {
    if (d.layer >= 0 && d.totalLayers > 0) {
      char layer[24];
      snprintf(layer, sizeof(layer), "層 %d/%d", d.layer, d.totalLayers);
      c->setTextDatum(middle_left);
      c->drawString(layer, 8, line1);
    }
    c->setTextDatum(middle_right);
    c->drawString(temps, kWidth - 8, line1);

    int etaW = 0;
    if (d.eta[0] != '\0') {
      char eta[24];
      snprintf(eta, sizeof(eta), "完成 %s", d.eta);
      c->setTextDatum(middle_right);
      c->drawString(eta, kWidth - 8, line2);
      etaW = c->textWidth(eta) + 12;
    }
    if (d.job[0] != '\0') {
      // ジョブ名は残り幅に収まる分だけ描く
      const int maxW = kWidth - 16 - etaW;
      char job[sizeof(d.job)];
      strncpy(job, d.job, sizeof(job));
      job[sizeof(job) - 1] = '\0';
      while (job[0] != '\0' && c->textWidth(job) > maxW) {
        size_t len = strlen(job);
        do {
          --len;
        } while (len > 0 && (static_cast<uint8_t>(job[len]) & 0xC0) == 0x80);
        job[len] = '\0';
      }
      c->setTextDatum(middle_left);
      c->drawString(job, 8, line2);
    }
  } else if (temps[0] != '\0') {
    c->setTextDatum(middle_center);
    c->drawString(temps, kWidth / 2, line1 + 8);
  }
}

int FaceHud::drawWrapped(M5Canvas* c, const char* text, int x, int y,
                         int maxWidth, int lineHeight, int skipLines,
                         int lines, uint16_t fg, uint16_t bg) {
  c->setTextColor(fg, bg);
  c->setTextDatum(top_left);
  if (lines == 0) {
    // Lay out once per caption. Rendering subsequent frames needs only two
    // complete drawString calls, with no per-glyph width measurements.
    strncpy(cachedCaption_, text, sizeof(cachedCaption_) - 1);
    cachedCaption_[sizeof(cachedCaption_) - 1] = '\0';
    int line = 0;
    int width = 0;
    lineStarts_[0] = 0;
    size_t pos = 0;
    while (cachedCaption_[pos]) {
      char glyph[5] = {};
      size_t n = 0;
      const size_t len = utf8Length(static_cast<uint8_t>(cachedCaption_[pos]));
      while (n < len && cachedCaption_[pos + n]) {
        glyph[n] = cachedCaption_[pos + n];
        ++n;
      }
      if (glyph[0] == '\n') {
        lineStarts_[++line] = pos + n;
        width = 0;
      } else {
        const int w = c->textWidth(glyph);
        if (width + w > maxWidth && width > 0 && !isClosingPunctuation(glyph)) {
          lineStarts_[++line] = pos;
          width = 0;
          if (glyph[0] == ' ') {
            lineStarts_[line] = pos + n;
            pos += n;
            continue;
          }
        }
        width += w;
      }
      pos += n;
    }
    lineStarts_[line + 1] = pos;
    cachedLines_ = line + 1;
    return cachedLines_;
  }
  char lineText[sizeof(cachedCaption_)];
  for (int i = skipLines; i < cachedLines_ && i < skipLines + lines; ++i) {
    const size_t begin = lineStarts_[i];
    size_t end = lineStarts_[i + 1];
    while (end > begin && (cachedCaption_[end - 1] == '\n' ||
                          cachedCaption_[end - 1] == '\r' ||
                          cachedCaption_[end - 1] == ' ')) --end;
    memcpy(lineText, cachedCaption_ + begin, end - begin);
    lineText[end - begin] = '\0';
    c->drawString(lineText, x, y + (i - skipLines) * lineHeight);
  }
  return cachedLines_;
}

void HudMouth::draw(M5Canvas* spi, m5avatar::BoundingRect rect,
                    m5avatar::DrawContext* ctx) {
  inner_->draw(spi, rect, ctx);
  const int depth = ctx->getColorDepth();
  const uint16_t fg =
      depth == 1 ? 1 : ctx->getColorPalette()->get(COLOR_PRIMARY);
  const uint16_t bg =
      depth == 1 ? 0 : ctx->getColorPalette()->get(COLOR_BACKGROUND);
  hud_->draw(spi, depth, fg, bg);
}
