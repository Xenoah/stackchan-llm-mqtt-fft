#pragma once

#include <Arduino.h>

// Bambu Lab プリンタ（P1S 等）の状態スナップショット。
//
// MQTT タスクが push_status を受信するたびに更新し、メインループ側は
// BambuMqttClient::snapshot() でコピーを取り出して使う（共有はコピー渡しのみ）。
// P1 シリーズは差分（変化したフィールドだけ）を送ってくるため、
// 各フィールドは「受信したときだけ上書き」して最新値を保持する。

// gcode_state を列挙化したもの
enum class PrintPhase : uint8_t {
  Unknown,   // まだ受信していない
  Idle,      // IDLE
  Prepare,   // PREPARE（加熱・レベリングなど印刷前処理）
  Slicing,   // SLICING（本体スライス中）
  Running,   // RUNNING（印刷中）
  Pause,     // PAUSE（一時停止）
  Finish,    // FINISH（完了）
  Failed,    // FAILED（失敗・キャンセル）
};

// 印刷速度モード（spd_lvl）
enum class SpeedLevel : uint8_t {
  Unknown = 0,
  Silent = 1,
  Standard = 2,
  Sport = 3,
  Ludicrous = 4,
};

// AMS トレイ1枠分の情報
struct AmsTray {
  bool present = false;     // フィラメントが入っているか
  char type[12] = {};       // "PLA" / "PETG" など
  uint32_t color = 0;       // RGB888（tray_color の先頭6桁）
  int8_t remain = -1;       // 残量 %（-1 = 不明）
};

// HMS（Health Management System）エラー1件
struct HmsEntry {
  uint32_t attr = 0;
  uint32_t code = 0;
};

// MQTT 接続状態
enum class LinkState : uint8_t {
  Disabled,     // 設定でOFF、または未設定
  WaitingWifi,  // Wi-Fi 未接続
  Connecting,   // TLS/MQTT 接続試行中
  Online,       // 接続済み・購読済み
  Error,        // 直近の接続に失敗（リトライ待ち）
};

struct PrinterState {
  static constexpr uint8_t kMaxAms = 4;
  static constexpr uint8_t kTraysPerAms = 4;
  static constexpr uint8_t kMaxHms = 6;

  // --- 接続 ---
  LinkState link = LinkState::Disabled;
  int mqttErrorCode = 0;        // PubSubClient::state()（接続失敗時）
  uint32_t lastMessageAt = 0;   // 最後に push_status を受けた millis()
  uint32_t messageCount = 0;    // 受信した push_status の数
  bool synced = false;          // gcode_state を1度でも受信したか（全体状態が揃った目安）

  // --- 印刷ジョブ ---
  PrintPhase phase = PrintPhase::Unknown;
  char gcodeState[12] = "";     // 生の gcode_state 文字列
  char jobName[48] = "";        // subtask_name（拡張子なしのファイル名）
  int percent = -1;             // mc_percent
  int remainingMin = -1;        // mc_remaining_time（分）
  int layer = -1;               // layer_num
  int totalLayers = -1;         // total_layer_num
  int stage = -1;               // stg_cur（現在の処理ステージ ID）
  uint32_t printError = 0;      // print_error（0 = なし）
  SpeedLevel speed = SpeedLevel::Unknown;

  // --- 温度 ---
  float nozzleTemp = NAN;
  float nozzleTarget = NAN;
  float bedTemp = NAN;
  float bedTarget = NAN;
  float chamberTemp = NAN;

  // --- ファン（0〜100%）---
  int partFan = -1;             // cooling_fan_speed
  int auxFan = -1;              // big_fan1_speed
  int chamberFan = -1;          // big_fan2_speed

  // --- その他 ---
  int wifiDbm = 0;              // wifi_signal（"-45dBm" → -45）
  int8_t chamberLight = -1;     // 1=on 0=off -1=不明

  // --- AMS ---
  uint8_t amsCount = 0;
  AmsTray trays[kMaxAms][kTraysPerAms];
  int trayNow = -1;             // 使用中トレイ（-1=なし, 254=外部スプール, それ以外=ams*4+tray）
  int amsHumidity[kMaxAms] = {-1, -1, -1, -1}; // 1〜5（5=乾燥）

  // --- HMS ---
  uint8_t hmsCount = 0;
  HmsEntry hms[kMaxHms];

  // 印刷ジョブが進行中か（準備・印刷・一時停止）
  bool isActive() const {
    return phase == PrintPhase::Prepare || phase == PrintPhase::Running ||
           phase == PrintPhase::Pause || phase == PrintPhase::Slicing;
  }
};

// 文字列の gcode_state を PrintPhase に変換する
PrintPhase printPhaseFromString(const char* value);

// 表示用の短いラベル（日本語）
const char* printPhaseLabelJa(PrintPhase phase);

// 英語ラベル（Web/シリアル用）
const char* printPhaseLabelEn(PrintPhase phase);

// stg_cur の日本語説明（不明な ID は nullptr）
const char* stageLabelJa(int stage);

// 速度モードの日本語名
const char* speedLabelJa(SpeedLevel level);

// HMS コードを "0300_0100_0001_0007" 形式に整形する
String hmsCodeString(const HmsEntry& entry);

// 残り時間（分）を "1時間23分" / "45分" 形式にする（音声・字幕用）
String remainingJa(int minutes);

// 残り時間（分）を "1:23" 形式にする（HUD 用）
String remainingShort(int minutes);

// RGB888 を大まかな日本語の色名にする（例: 0xFF0000 → "赤"）
const char* colorNameJa(uint32_t rgb);

// print_error がユーザーキャンセルを示すか
bool isCancelError(uint32_t printError);
