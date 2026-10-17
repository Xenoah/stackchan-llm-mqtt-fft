#pragma once

#include <Arduino.h>
#include <Drawable.h>

// 顔の上に重ねるプリンタ HUD（ヘッドアップ表示）の表示内容。
// メインループが set() で書き込み、アバター描画タスクが draw() で読む。
struct HudData {
  bool visible = false;      // プリンター情報の表示（字幕は独立）
  bool active = false;       // 印刷ジョブ進行中（進捗バー・残り時間を出す）
  bool alert = false;        // エラー/お知らせあり（状態チップを点滅）
  char phase[28] = "";       // 状態チップの文字（例: "印刷中"）
  int percent = -1;          // 進捗（-1 = 不明）
  char remaining[12] = "";   // 残り時間（例: "1:23"）
  char eta[12] = "";         // 完成予定（例: "15:42"）
  char job[48] = "";         // ジョブ名
  int layer = -1;
  int totalLayers = -1;
  int nozzle = INT16_MIN;    // ℃（INT16_MIN = 不明）
  int bed = INT16_MIN;
};

// 顔スプライトに HUD を描く。
// m5avatar は 320x240 の 1bit スプライトへ顔パーツを描いてから LCD へ転送するので、
// 同じスプライトへ描けば描画タスク間で LCD を取り合わずに済む（ちらつきも出ない）。
class FaceHud {
 public:
  FaceHud();

  // HUD の内容を更新する（メインループから）
  void set(const HudData& data);

  // 字幕（実況テキスト）を表示する。durationMs=0 で clearCaption() まで表示
  void setCaption(const String& text, uint32_t durationMs);
  void clearCaption();
  bool hasCaption();  // 有効期限内の字幕があるか（プリンター情報のON/OFFとは独立）
  void setSpectrum(bool visible, const uint8_t (&bars)[32]);
  bool hasSpectrum();

  // 短いステータス表示（"TTS" など）。字幕が無いときだけ下部に出す
  void setToast(const char* text, uint32_t durationMs);

  bool isVisible() const { return data_.visible; }

  // 顔スプライトへ描く（アバター描画タスクから）
  void draw(M5Canvas* canvas, int colorDepth, uint16_t fg, uint16_t bg);

 private:
  void drawTopBar(M5Canvas* c, const HudData& d, uint16_t fg, uint16_t bg,
                  uint32_t now);
  void drawBottom(M5Canvas* c, const HudData& d, const char* caption,
                  uint32_t captionSince, const char* toast, uint16_t fg,
                  uint16_t bg, uint32_t now);
  // UTF-8 テキストを幅 maxWidth で折り返して lines 行ぶん描く。
  // skipLines 行ぶんは描かずに読み飛ばす。戻り値は全行数。
  int drawWrapped(M5Canvas* c, const char* text, int x, int y, int maxWidth,
                  int lineHeight, int skipLines, int lines, uint16_t fg,
                  uint16_t bg);

  portMUX_TYPE mux_;
  HudData data_;
  bool spectrumVisible_ = false;
  uint8_t spectrumBars_[32] = {};
  char caption_[192] = "";
  uint32_t captionSince_ = 0;
  uint32_t captionUntil_ = 0;  // 0 = 無期限
  char toast_[24] = "";
  uint32_t toastUntil_ = 0;

  // 字幕の行数キャッシュ（描画タスクだけが触る）
  uint32_t cachedSince_ = UINT32_MAX;
  int cachedLines_ = 1;
  char cachedCaption_[192] = "";
  uint8_t lineStarts_[193] = {};  // byte offsets; caption is at most 191 bytes
};

// 顔の口パーツを包み、口を描いたあとに HUD を重ねる Drawable。
// Face には HUD 用の差し込み口が無いため、全顔テンプレートにある「口」を利用する。
class HudMouth : public m5avatar::Drawable {
 public:
  HudMouth(m5avatar::Drawable* inner, FaceHud* hud) : inner_(inner), hud_(hud) {}
  ~HudMouth() override { delete inner_; }

  void draw(M5Canvas* spi, m5avatar::BoundingRect rect,
            m5avatar::DrawContext* ctx) override;

 private:
  m5avatar::Drawable* inner_;
  FaceHud* hud_;
};
