#pragma once

#include <Arduino.h>

#include "PrinterState.h"

// 実況の表情ヒント（main 側で m5avatar::Expression に対応付ける）
enum class CommentMood : uint8_t {
  Neutral,
  Happy,
  Sad,
  Doubt,
  Angry,
  Sleepy,
};

// 実況の優先度。High は待ち行列の低優先コメントを押しのけて先頭に入る。
enum class CommentPriority : uint8_t {
  Low,     // 工程・温度・定期報告など（詰まっていたら捨てる）
  Normal,  // 進捗・開始など
  High,    // 完了・失敗・エラー・一時停止
};

struct Comment {
  String text;       // 字幕（画面・Web 表示用）
  String speech;     // 読み上げ用（空なら text を読む）
  CommentMood mood = CommentMood::Neutral;
  CommentPriority priority = CommentPriority::Normal;
  bool celebrate = false;  // 完了時の喜びモーション
  uint32_t createdAt = 0;

  const String& spoken() const { return speech.isEmpty() ? text : speech; }
};

// 実況の設定（NVS に保存され、Web 設定画面から変更する）
struct CommentarySettings {
  uint8_t progressStep = 10;    // 何 % ごとに進捗を実況するか（0 = しない）
  uint16_t periodicMin = 15;    // 無言が続いたときの定期報告間隔（分、0 = しない）
  bool stages = true;           // 準備中の工程（レベリング・加熱など）を実況
  bool temps = true;            // 目標温度到達を実況
};

// プリンタ状態の変化から実況コメントを生成するエンジン。
//
// update() に最新スナップショットを渡すと、前回との差分からイベントを検出し、
// 優先度付きの待ち行列にコメントを積む。main 側は発話できるタイミングで
// popComment() して喋る。スレッドはメインループのみ。
class PrintCommentator {
 public:
  void configure(const CommentarySettings& settings);
  const CommentarySettings& settings() const { return settings_; }

  // 状態の差分からコメントを生成する（状態が変わったとき・定期的に呼ぶ）
  void update(const PrinterState& state, uint32_t now);

  // 発話待ちのコメントがあるか
  bool hasComment() const { return count_ > 0; }

  // 先頭のコメントの優先度（無ければ Low）
  CommentPriority peekPriority() const;

  // 先頭のコメントを取り出す
  Comment popComment();

  // 手動のコメント（Web からの状況報告・テスト発話）を待ち行列に積む。
  // HTTP ハンドラは TTS 再生中にも呼ばれるため、その場では喋らずここへ積む。
  void enqueue(const Comment& comment) { push(comment); }

  // いまの状況を1文でまとめる（頭タッチ・Web ボタン用）
  Comment statusReport(const PrinterState& state) const;

  // 最近のコメント履歴（Web 表示用、新しい順に index 0..）
  static constexpr uint8_t kHistorySize = 12;
  uint8_t historyCount() const { return historyCount_; }
  const Comment& history(uint8_t index) const;

  // 手動で履歴に積む（テスト発話や状況報告を Web に残す用）
  void remember(const Comment& comment);

 private:
  static constexpr uint8_t kQueueSize = 5;

  void push(Comment comment);
  void say(CommentPriority priority, CommentMood mood, const String& text,
           const String& speech = String(), bool celebrate = false);

  void resetJob();
  void onFirstSync(const PrinterState& s);
  void onPhaseChange(const PrinterState& prev, const PrinterState& s,
                     uint32_t now);
  void onStageChange(const PrinterState& prev, const PrinterState& s);
  void onProgress(const PrinterState& prev, const PrinterState& s);
  void onTemperatures(const PrinterState& prev, const PrinterState& s);
  void onFilament(const PrinterState& prev, const PrinterState& s);
  void onErrors(const PrinterState& prev, const PrinterState& s);
  void onLink(const PrinterState& s, uint32_t now);
  void onPeriodic(const PrinterState& s, uint32_t now);

  CommentarySettings settings_;
  PrinterState prev_;
  bool initialized_ = false;

  // 進捗
  int lastMilestone_ = 0;
  bool firstLayerAnnounced_ = false;
  bool lastLayerAnnounced_ = false;
  bool tenMinutesAnnounced_ = false;
  bool bedReached_ = false;
  bool nozzleReached_ = false;
  uint64_t announcedStages_ = 0;   // このジョブで実況済みの stg_cur（ビット）
  uint32_t jobStartedAt_ = 0;      // RUNNING に入った millis()（0 = 未観測）
  uint32_t lastCommentAt_ = 0;     // 最後にコメントを積んだ millis()
  uint32_t pendingPauseAt_ = 0;    // 一時停止の実況待ち（0 = なし）
  uint32_t lastFilamentCommentAt_ = 0;
  uint32_t lastHmsSignature_ = 0;
  uint32_t lastHmsAt_ = 0;

  // 接続
  bool everOnline_ = false;
  bool neverOnlineAnnounced_ = false;
  uint32_t offlineSince_ = 0;
  bool offlineAnnounced_ = false;

  // 待ち行列（リングバッファ）
  Comment queue_[kQueueSize];
  uint8_t head_ = 0;
  uint8_t count_ = 0;

  // 履歴（リングバッファ）
  Comment history_[kHistorySize];
  uint8_t historyHead_ = 0;
  uint8_t historyCount_ = 0;
};

// 読み上げ用にジョブ名を整える（長さ制限・記号の置き換え）
String speakableJobName(const char* jobName);

// 終了予定時刻を "15時42分" 形式で返す（時計未同期なら空文字）
String etaClockJa(int remainingMin);
