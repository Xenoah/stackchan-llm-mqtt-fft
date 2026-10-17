// StackChan Codex - メインエントリポイント
//
// M5Stack CoreS3 + StackChanボディを使ったデスクロボットのメインファームウェア。
//
// 主な機能:
//   - VoiceVox互換またはSimpleWav TTSサーバへのWiFi経由音声合成
//   - アバターフェース（表情・顔型・カラーパレット・目パターン・変形）
//   - PCM波形解析によるリアルタイム口パク同期（30ms間隔）
//   - LEVEL HOLDモード: IMU加速度を使ったPID制御でサーボを水平維持
//   - タッチスクリーンの下スワイプでモード・設定メニューを開く
//   - 頭部タッチセンサで表情・パレット等を操作
//   - WiFiでアクセス可能な設定Webサーバ（ポート80）
//   - Bambu Lab プリンタ（P1S 等）の LAN MQTT 監視と、スタックチャンによる実況
//   - 顔に重ねるプリンタ HUD、本体 LED の進捗表示、本体のプリンタ詳細画面

#include <M5Unified.h>
#include <esp_camera.h>
#include <esp_system.h>
#include <math.h>
#include <time.h>

#include "AvatarFaceController.h"
#include "BambuMqttClient.h"
#include "BuiltinVoice.h"
#include "CalibrationController.h"
#include "ConfigPortal.h"
#include "PetReaction.h"
#include "PrintCommentator.h"
#include "PrinterJson.h"
#include "PrinterScreen.h"
#include "SetupScreens.h"
#include "SetupUi.h"
#include "Version.h"
#include "UsbSpectrum.h"
#include "usb/UsbAudio.h"
#include "VoiceVoxClient.h"
#include "hardware_features.h"

// グローバルオブジェクト（各サブシステムのコントローラ）
AvatarFaceController avatarFace;          // アバター描画・アニメーション管理
CalibrationController calibrationController; // サーボ・IMUキャリブレーション
ConfigPortal configPortal;               // WiFi接続・設定Webサーバ
TtsClient ttsClient;                     // TTS音声合成クライアント
BuiltinVoice builtinVoice;               // 内蔵ボイス（TTS サーバーなしで喋る）
uint32_t ttsServerDownAt = 0;            // TTS サーバーに失敗した時刻（0 = 正常）
constexpr uint32_t kTtsServerRetryMs = 3UL * 60UL * 1000UL; // 失敗後は3分間 内蔵ボイスを使う
constexpr char kBuiltinGreeting[] = "こんにちは！スタックチャンだよ";
BambuMqttClient bambu;                   // Bambu Lab プリンタ LAN MQTT（専用タスク）
PrintCommentator commentator;            // 印刷状況の実況エンジン

// --- プリンタ監視・実況の状態 ---
PrinterState printerNow;                 // 最新のプリンタ状態（メインループ用のコピー）
uint32_t printerRevisionSeen = 0;        // 取り込み済みの状態リビジョン
uint32_t printerEvaluatedAt = 0;         // 最後に実況判定した時刻
PrintPhase printerLastPhase = PrintPhase::Unknown;
uint32_t printerPhaseSince = 0;          // 現在の gcode_state になった時刻
uint32_t commentBusyUntil = 0;           // この時刻までは次の実況を始めない
String lastCommentText;                  // 最後の実況（本体詳細画面に表示）
bool printerScreenOpen = false;          // 本体のプリンタ詳細画面を表示中か
uint32_t printerScreenDrawnAt = 0;
uint32_t printerScreenRevision = 0;
PrinterScreenRenderer printerScreenRenderer;
constexpr uint32_t kCommentGapMs = 1500;                   // 実況と実況の間
constexpr uint32_t kPhaseLedHoldMs = 5UL * 60UL * 1000UL;  // 完了/失敗の LED 表示時間

// --- MQTT モード（プリンター実況）への自動切り替え ---
// 印刷が始まったら MQTT モードへ移り、終わって5分たったら元のモードへ戻る。
// 印刷中に手動で別のモードへ移ったら、そのジョブの間は自動で戻さない。
bool printerModeAuto = false;            // 自動で MQTT モードに入った（終わったら元へ戻す）
bool autoModeHeldOff = false;            // このジョブの間は自動で入らない
uint32_t printerIdleSince = 0;           // 印刷が終わった（進行中でなくなった）時刻
constexpr uint32_t kAutoModeReturnMs = kPhaseLedHoldMs;
volatile int8_t pendingModeRequest = -1; // Web からのモード切り替え（AppMode の値、-1 = なし）

void updatePrinterMonitor(uint32_t now);
void updatePrinterHud(const PrinterState& s);
void closePrinterScreen();
void showPrinterScreenIfFree();
void openPrinterScreen();
void updatePetInteraction();
bool updatePrinterLed(uint32_t now);
String ttsTextFor(const String& text);

// 口パク同期の設定
constexpr uint32_t LIP_FRAME_INTERVAL_MS = 30;  // 口パク更新間隔（30ms）
constexpr uint32_t LIP_INPUT_TIMEOUT_MS = 220;  // 入力がなければ口を閉じるまでの時間

// 口パク同期の状態変数
int currentMouthOpen = 0;         // 現在の口の開き度合い（0〜100）
int targetMouthOpen = 0;          // 目標の口の開き度合い（TTSから設定）
bool lipSyncActive = false;       // 口パク同期が動作中かどうか
bool speaking = false;            // TTSが話し中かどうか
PetReaction petReaction;
bool petInteractionReady = false;  // 起動・校正中は頭を撫でても操作しない
bool petFeedbackPending = false;
bool petSpeaking = false;
bool petRestorePrinterScreen = false;

// Gateway からの POST /api/speak 受付用。
// WebServer ハンドラ内では再生せず（ブロッキングのため）、フラグを立てて
// loop() が拾って実際の発話を行う。pendingApiSpeakText が空なら "__CURRENT__" を発話する。
bool pendingApiSpeak = false;     // /api/speak の発話待ちがあるか
String pendingApiSpeakText;       // /api/speak で指定された発話テキスト（空=__CURRENT__）

// --- 診断（クラッシュ調査用）---
// 「しばらく動かすと落ちる」の原因切り分けのため、再起動理由とヒープ推移を記録する。
String bootResetReason = "?";     // 起動時に記録した直近の再起動理由
uint32_t minFreeHeapEver = UINT32_MAX; // 起動以降に観測した最小空きヒープ

// esp_reset_reason_t を人間が読めるラベルに変換する。
// PANIC=コードのクラッシュ、BROWNOUT=電源不足、TASK/INT WDT=ハング、POWERON=通常。
const char* resetReasonLabel(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNKNOWN";
  }
}

// --- カメラ目線（明るい方向へ視線を向ける）---
// CoreS3 内蔵カメラ(GC0308)で低解像度フレームを取得し、輝度の重心方向へ目を向ける。
// カメラとタッチは内部I2Cバスを共有するため、設定(cameraGaze)でON/OFFできる。
extern bool modeMenuOpen;          // メニュー表示状態（定義は後方）
extern bool settingsInfoOpen;      // 設定情報画面の表示状態（定義は後方）

bool cameraGazeAvailable = false;  // カメラ初期化に成功したか
bool cameraGazeActive = false;     // 実際に視線制御中か
uint32_t lastCameraGazeAt = 0;     // 最後にフレーム解析した時刻
float camGazeSmoothH = 0.0f;       // 平滑化した水平視線
float camGazeSmoothV = 0.0f;       // 平滑化した垂直視線

// カメラ目線のチューニング
constexpr uint32_t kCamGazeIntervalMs = 250; // フレーム解析間隔（4fps）
constexpr float kCamGazeGain = 1.6f;          // 視線の振り幅（大げさに）
constexpr float kCamGazeSmoothing = 0.35f;    // 平滑化係数（0=なめらか）
constexpr int kCamGazeHSign = 1;              // 水平の符号（左右が逆なら -1 にする）

// GC0308 カメラ設定（M5CoreS3 ライブラリの設定を基に QQVGA で軽量化）
camera_config_t cameraGazeConfig = {
    .pin_pwdn = -1, .pin_reset = -1, .pin_xclk = -1,
    .pin_sscb_sda = 12, .pin_sscb_scl = 11,
    .pin_d7 = 47, .pin_d6 = 48, .pin_d5 = 16, .pin_d4 = 15,
    .pin_d3 = 42, .pin_d2 = 41, .pin_d1 = 40, .pin_d0 = 39,
    .pin_vsync = 46, .pin_href = 38, .pin_pclk = 45,
    .xclk_freq_hz = 20000000,
    .ledc_timer = LEDC_TIMER_0, .ledc_channel = LEDC_CHANNEL_0,
    .pixel_format = PIXFORMAT_RGB565,
    .frame_size = FRAMESIZE_QQVGA,  // 160x120（軽量）
    .jpeg_quality = 0, .fb_count = 2,
    .fb_location = CAMERA_FB_IN_PSRAM,
    .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
    .sccb_i2c_port = -1,
};

// カメラを初期化する（config の cameraGaze が true のときだけ setup() から呼ぶ）。
void setupCameraGaze() {
  M5.In_I2C.release(); // 内部I2Cを解放してカメラSCCBに渡す
  const esp_err_t err = esp_camera_init(&cameraGazeConfig);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x (camera gaze disabled)\n", err);
    cameraGazeAvailable = false;
    return;
  }
  cameraGazeAvailable = true;
  Serial.println("Camera gaze ready");
}

// 1フレーム取得して輝度重心の方向へ視線を向ける（throttle 付き・非ブロッキング）。
void updateCameraGaze() {
  if (!cameraGazeAvailable) {
    return;
  }
  // 発話中・メニュー表示中は I2C/CPU 競合を避けて休む
  if (speaking || modeMenuOpen || settingsInfoOpen || printerScreenOpen || usb_spectrum::isOpen()) {
    return;
  }
  const uint32_t now = millis();
  if (now - lastCameraGazeAt < kCamGazeIntervalMs) {
    return;
  }
  lastCameraGazeAt = now;

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb || fb->format != PIXFORMAT_RGB565) {
    if (fb) esp_camera_fb_return(fb);
    return;
  }

  // RGB565（リトルエンディアン）から近似輝度を計算し、重心を求める。
  // 負荷軽減のため縦横ともに間引いてサンプリングする。
  const int W = fb->width;
  const int H = fb->height;
  const uint8_t* p = fb->buf;
  const int stepX = 4;
  const int stepY = 4;
  uint64_t sumLum = 0, sumX = 0, sumY = 0;
  for (int y = 0; y < H; y += stepY) {
    const uint8_t* row = p + (size_t)y * W * 2;
    for (int x = 0; x < W; x += stepX) {
      const uint16_t px = row[x * 2] | (row[x * 2 + 1] << 8);
      const int r = (px >> 11) & 0x1F;
      const int g = (px >> 5) & 0x3F;
      const int b = px & 0x1F;
      // 近似輝度（5/6/5bit を概ね揃える）
      const uint32_t lum = (uint32_t)(r << 1) + g + (uint32_t)(b << 1);
      sumLum += lum;
      sumX += (uint64_t)lum * x;
      sumY += (uint64_t)lum * y;
    }
  }
  esp_camera_fb_return(fb);

  if (sumLum == 0) {
    return; // 真っ暗：視線は維持
  }

  // 重心を 0..1 に正規化 → -1..1 の視線へ
  const float cx = (float)sumX / (float)sumLum / (float)W; // 0..1
  const float cy = (float)sumY / (float)sumLum / (float)H; // 0..1
  float h = (cx - 0.5f) * 2.0f * kCamGazeGain * kCamGazeHSign;
  float v = (cy - 0.5f) * 2.0f * kCamGazeGain; // 上(y小)=負=上向き

  // 平滑化（急な動きを抑える）
  camGazeSmoothH += (h - camGazeSmoothH) * kCamGazeSmoothing;
  camGazeSmoothV += (v - camGazeSmoothV) * kCamGazeSmoothing;

  avatarFace.setGaze(camGazeSmoothV, camGazeSmoothH);
  cameraGazeActive = true;
}
uint32_t lipSyncLastInputAt = 0;  // 最後に口パク入力があった時刻
uint32_t lipSyncLastFrameAt = 0;  // 最後に口パクフレームを更新した時刻

// 起動時サーボ選択ダイアログの結果
enum class ServoStartupChoice {
  KeepPosition, // キャリブレーションせずに現在位置を維持
  Calibrate,    // フルキャリブレーションを実行
};

// 起動時サーボダイアログのボタン判定結果
enum class ServoPromptButton {
  None,
  No,
  Yes,
};

// アプリの動作モード
enum class AppMode {
  LocalLlm,  // 通常モード: TTSサーバと通信して音声合成
  LevelHold, // 水平維持モード: IMUでサーボを安定化
  Printer,   // MQTT モード: プリンター実況（顔に HUD、頭タップで状況報告）
};

// 下スワイプメニューのボタン
enum class ModeMenuButton {
  None,
  PrinterMode, // MQTT モード（プリンター実況）に切り替え
  Printer,   // プリンタ詳細画面を開く
  Voice,     // 実況の声 ON/OFF
  LocalLlm,  // LOCAL LLMモードに切り替え
  LevelHold, // LEVEL HOLDモードに切り替え
  Settings,  // 設定（Wi-Fi・プリンター・接続情報）を開く
  Fft,       // USBオーディオのスペクトラム画面
  Close,     // メニューを閉じる
};

// PID（比例-積分-微分）コントローラ。
// LEVEL HOLDモードでサーボを水平に維持するために使用する。
struct PidController {
  float kp = 0.0f;          // 比例ゲイン: 誤差に対する応答の強さ
  float ki = 0.0f;          // 積分ゲイン: 持続する誤差を消去する（このアプリでは0）
  float kd = 0.0f;          // 微分ゲイン: 急な変化を抑制するダンパー効果
  float integral = 0.0f;    // 積分項の累積値
  float previousError = 0.0f; // 前回の誤差（微分計算に使用）
  bool hasPrevious = false;   // 前回誤差があるかどうか（最初のフレームでは微分を0にする）

  // 状態をリセットする（モード切り替え時などに呼ぶ）
  void reset() {
    integral = 0.0f;
    previousError = 0.0f;
    hasPrevious = false;
  }

  // 誤差と経過時間からPID出力を計算する
  float update(float error, float dt) {
    integral += error * dt;
    integral = constrain(integral, -80.0f, 80.0f); // 積分飽和を防ぐ
    const float derivative =
        hasPrevious && dt > 0.0f ? (error - previousError) / dt : 0.0f;
    previousError = error;
    hasPrevious = true;
    return kp * error + ki * integral + kd * derivative;
  }
};

// デッドバンド処理: 小さな誤差（±deadband以内）を0に丸める。
// これにより、サーボのわずかな揺れ（ジッタ）を防ぐ。
float applyDeadband(float value, float deadband) {
  if (fabsf(value) <= deadband) {
    return 0.0f;
  }
  return value > 0.0f ? value - deadband : value + deadband;
}

// current を target に向かって最大 maxStep ずつ近づける。
// 急激な位置変化によるサーボへの衝撃を防ぐレートリミッタ。
int moveTowardByStep(int current, int target, int maxStep) {
  if (target > current) {
    return min(current + maxStep, target);
  }
  if (target < current) {
    return max(current - maxStep, target);
  }
  return current;
}

// 文字列からTTSエンジン種別に変換する
TtsEngineType ttsEngineTypeFromString(const String& value) {
  return value == "simple_wav" ? TtsEngineType::SimpleWav
                               : TtsEngineType::VoiceVoxCompatible;
}

// 起動時UIや各種メニューで使う共有キャンバスを返す。
// 画面サイズが変わった場合（回転後など）はキャンバスを再作成する。
// フリッカーフリー描画のために、描画→pushSprite()の順で使う。
M5Canvas& startupCanvas() {
  static M5Canvas canvas(&M5.Display);
  static int16_t canvasWidth = 0;
  static int16_t canvasHeight = 0;

  const int16_t width = M5.Display.width();
  const int16_t height = M5.Display.height();
  if (canvasWidth != width || canvasHeight != height) {
    canvas.deleteSprite();
    canvas.setColorDepth(8);
    // RGB332: 320x240 のUIを75KiBにする（16bit時の半分）。TLS用RAMは使わない。
    canvas.setPsram(true);
    canvas.createSprite(width, height);
    canvasWidth = width;
    canvasHeight = height;
  }
  return canvas;
}

// LEVEL HOLDモードの各種定数
constexpr uint32_t LEVEL_HOLD_UPDATE_INTERVAL_MS = 80;  // PID更新間隔（80ms）
constexpr int LEVEL_HOLD_SERVO_SPEED = 160;              // サーボ移動速度
constexpr float LEVEL_HOLD_FILTER_ALPHA = 0.35f;         // ローパスフィルタ係数（0に近いほど平滑化）
constexpr float LEVEL_HOLD_DEADBAND_DEG = 0.6f;          // ジッタ防止デッドバンド（±0.6度）
constexpr float LEVEL_HOLD_MAX_OUTPUT_DEG = 35.0f;       // PID出力の最大値（±35度）
constexpr int LEVEL_HOLD_MAX_STEP_DEG = 5;               // 1フレームの最大移動量（5度）
constexpr float LEVEL_HOLD_ACCEL_MIN_NORM = 0.05f;       // 有効な加速度ベクトルの最小長さ

// アプリの状態変数
AppMode currentMode = AppMode::LocalLlm; // 現在の動作モード
AppMode modeBeforeAuto = AppMode::LocalLlm; // 自動で MQTT モードに入る前のモード
bool levelHoldActive = false;            // LEVEL HOLDが有効かどうか
uint32_t levelHoldLastUpdateAt = 0;      // 最後にPIDを更新した時刻

// LEVEL HOLD用PIDコントローラ（ロール・ピッチそれぞれ独立）
// kp=1.15/1.35: 応答感度、kd=0.12/0.14: ダンパー効果（ki=0.0: 積分なし）
PidController levelHoldRollPid{1.15f, 0.0f, 0.12f};
PidController levelHoldPitchPid{1.35f, 0.0f, 0.14f};

// ローパスフィルタの状態変数
bool levelHoldFilterReady = false;         // フィルタが初期化済みかどうか
float levelHoldFilteredRollDeg = 0.0f;    // フィルタ後のロール角（度）
float levelHoldFilteredPitchDeg = 0.0f;   // フィルタ後のピッチ角（度）
int levelHoldYawTarget = 0;   // 現在のサーボヨー目標値（レートリミット後）
int levelHoldPitchTarget = 0; // 現在のサーボピッチ目標値（レートリミット後）

// 通常モード用の小さな身体モーション
enum class BodyMotionState {
  Stopped,
  Idle,
  Joy,
  Pet,
};

constexpr uint32_t BODY_MOTION_UPDATE_INTERVAL_MS = 80;
constexpr int BODY_IDLE_SERVO_SPEED = 160;       // 16.0度/秒
constexpr int BODY_JOY_SERVO_SPEED = 720;        // 72.0度/秒
constexpr int BODY_HOME_YAW = 0;                 // 正面
constexpr int BODY_HOME_PITCH = 200;             // 20.0度上向き
constexpr int BODY_IDLE_YAW_AMPLITUDE = 18;      // 1.8度
constexpr int BODY_IDLE_PITCH_AMPLITUDE = 7;     // 0.7度
constexpr int BODY_JOY_YAW_AMPLITUDE = 300;      // 30.0度
constexpr uint32_t BODY_JOY_DURATION_MS = 4800;  // 左右3往復を柔らかく
constexpr float BODY_JOY_CYCLES = 3.0f;
constexpr int BODY_MOTION_MAX_STEP = 24;         // 1更新あたり2.4度

BodyMotionState bodyMotionState = BodyMotionState::Stopped;
bool bodyMotionSkipAutoStart = false;
uint32_t bodyMotionStartedAt = 0;
uint32_t bodyMotionLastUpdateAt = 0;
int bodyMotionYawBase = 0;
int bodyMotionPitchBase = 0;
int bodyMotionYawTarget = 0;
int bodyMotionPitchTarget = 0;
int bodyJoyCenterYaw = 0;
int bodyJoyCenterPitch = 0;

// メニューとSettings情報画面の状態変数
bool modeMenuOpen = false;          // モードメニューが開いているかどうか
bool settingsInfoOpen = false;      // SETTINGS情報画面が開いているかどうか
bool deviceSetupOpen = false;       // 本体の設定画面（キーボード）を表示中か
bool displayWasTouching = false;    // 前フレームでタッチされていたかどうか
int16_t displayTouchStartX = 0;     // スワイプ開始X座標
int16_t displayTouchStartY = 0;     // スワイプ開始Y座標
int16_t displayTouchLastX = 0;      // スワイプ現在X座標
int16_t displayTouchLastY = 0;      // スワイプ現在Y座標
uint32_t displayTouchStartedAt = 0; // スワイプ開始時刻
ModeMenuButton modeMenuPressed = ModeMenuButton::None; // 押下中のメニューボタン
String modeMenuHint;                // メニューの見出しに少しだけ出すお知らせ
uint32_t modeMenuHintUntil = 0;     // お知らせを消す時刻（0 = 出していない）

// モード名の文字列を返す（ステータス表示用）
const char* appModeName(AppMode mode) {
  switch (mode) {
    case AppMode::LevelHold: return "LEVEL HOLD";
    case AppMode::Printer:   return "MQTT";
    default:                 return "LOCAL LLM";
  }
}

// Web API 用のモード名（/api/mode・/api/printer）
const char* appModeKey(AppMode mode) {
  switch (mode) {
    case AppMode::LevelHold: return "level";
    case AppMode::Printer:   return "mqtt";
    default:                 return "llm";
  }
}

// 首のアイドル動作・喜びモーションを使うモード（LEVEL HOLD はサーボを水平維持に使う）
bool modeUsesBodyMotion() {
  return currentMode == AppMode::LocalLlm || currentMode == AppMode::Printer;
}

// LEVEL HOLDモードのPIDとフィルタ状態をリセットする
void resetLevelHoldPid() {
  levelHoldRollPid.reset();
  levelHoldPitchPid.reset();
  levelHoldFilterReady = false;
  levelHoldFilteredRollDeg = 0.0f;
  levelHoldFilteredPitchDeg = 0.0f;
  levelHoldYawTarget = 0;
  levelHoldPitchTarget = 0;
  levelHoldLastUpdateAt = millis();
}

int clampBodyYaw(int value) {
  const auto& calibration = calibrationController.data();
  return constrain(value, calibration.yawMin, calibration.yawMax);
}

int clampBodyPitch(int value) {
  const auto& calibration = calibrationController.data();
  return constrain(value, calibration.pitchMin, calibration.pitchMax);
}

int bodyHomeYaw() {
  return clampBodyYaw(BODY_HOME_YAW);
}

int bodyHomePitch() {
  return clampBodyPitch(BODY_HOME_PITCH);
}

bool bodyMotionCanUseServo() {
  return calibrationController.data().servoValid &&
         modeUsesBodyMotion() &&
         !levelHoldActive &&
         !configPortal.isPortalActive() &&
         !modeMenuOpen &&
         !settingsInfoOpen &&
         !deviceSetupOpen &&
         (!speaking || (petSpeaking && bodyMotionState == BodyMotionState::Pet));
}

void stopBodyMotion(bool releaseServos) {
  if (bodyMotionState == BodyMotionState::Stopped) {
    return;
  }

  M5StackChan.Motion.stop();
  bodyMotionState = BodyMotionState::Stopped;

  if (releaseServos) {
    delay(30);
    M5StackChan.Motion.setTorqueEnabled(false);
    M5StackChan.Motion.setAutoAngleSyncEnabled(true);
    M5StackChan.Motion.setAutoTorqueReleaseEnabled(true);
    M5StackChan.setServoPowerEnabled(false);
  }
}

bool startBodyMotion(bool ignoreAutoStartSkip = false) {
  if (!calibrationController.data().servoValid ||
      !modeUsesBodyMotion() ||
      levelHoldActive ||
      (!ignoreAutoStartSkip && bodyMotionSkipAutoStart)) {
    return false;
  }

  // LOCAL LLM and MQTT share the same controller. A mode change must not
  // re-enable torque or replace its current trajectory with a UART read.
  if (bodyMotionState != BodyMotionState::Stopped) {
    Serial.printf("[motion] retained on mode change: target=(%d,%d)\n",
                  bodyMotionYawTarget, bodyMotionPitchTarget);
    return true;
  }

  M5StackChan.setServoPowerEnabled(true);
  delay(80);
  M5StackChan.Motion.setAutoAngleSyncEnabled(true);
  M5StackChan.Motion.setAutoTorqueReleaseEnabled(false);
  M5StackChan.Motion.setTorqueEnabled(true);
  delay(40);

  const auto angles = M5StackChan.Motion.getCurrentAngles();
  bodyMotionYawBase = bodyHomeYaw();
  bodyMotionPitchBase = bodyHomePitch();
  bodyMotionYawTarget = clampBodyYaw(angles.x);
  bodyMotionPitchTarget = clampBodyPitch(angles.y);
  bodyMotionStartedAt = millis();
  bodyMotionLastUpdateAt = 0;
  bodyMotionState = BodyMotionState::Idle;

  M5StackChan.Motion.move(
      bodyMotionYawTarget, bodyMotionPitchTarget, BODY_IDLE_SERVO_SPEED);
  // Synchronize once on startup; continuous targets keep the spring's current
  // position/velocity. Resyncing every 80ms amplified transient UART failures.
  M5StackChan.Motion.setAutoAngleSyncEnabled(false);
  Serial.printf(
      "Body motion idle started current=(%d,%d) home=(%d,%d)\n",
      bodyMotionYawTarget, bodyMotionPitchTarget,
      bodyMotionYawBase, bodyMotionPitchBase);
  return true;
}

// --- ゲーミングRGB（本体LED）---
// この時刻まではステータス色を保持し、虹色サイクルを一時停止する。
uint32_t gamingLedHoldUntil = 0;
// 最後に虹色LEDを更新した時刻（throttle用）
uint32_t gamingLedLastUpdateAt = 0;

// HSV(0..1) を RGB(0..255) に変換する（LED用）。
void hsvToRgb888(float h, float s, float v, uint8_t& r, uint8_t& g,
                 uint8_t& b) {
  h -= floorf(h);
  const float hf = h * 6.0f;
  const int i = static_cast<int>(hf) % 6;
  const float f = hf - floorf(hf);
  const float p = v * (1.0f - s);
  const float q = v * (1.0f - f * s);
  const float t = v * (1.0f - (1.0f - f) * s);
  float rf = 0, gf = 0, bf = 0;
  switch (i) {
    case 0: rf = v; gf = t; bf = p; break;
    case 1: rf = q; gf = v; bf = p; break;
    case 2: rf = p; gf = v; bf = t; break;
    case 3: rf = p; gf = q; bf = v; break;
    case 4: rf = t; gf = p; bf = v; break;
    default: rf = v; gf = p; bf = q; break;
  }
  r = static_cast<uint8_t>(rf * 255.0f);
  g = static_cast<uint8_t>(gf * 255.0f);
  b = static_cast<uint8_t>(bf * 255.0f);
}

// ステータス色をLEDに表示する。ゲーミングRGB有効時は holdMs の間その色を
// 保持してから虹色サイクルへ戻る（TTS/エラー等の状態表示を残すため）。
void showStatusLed(uint8_t r, uint8_t g, uint8_t b,
                   uint32_t holdMs = 1800) {
  M5StackChan.showRgbColor(r, g, b);
  gamingLedHoldUntil = millis() + holdMs;
}

// ゲーミングRGB有効時に、本体LEDを虹色でゆっくり循環させる（serviceApp から毎フレーム）。
// 顔(avatarFace)の虹色フェーズに色相を合わせて、画面とLEDの色を揃える。
void updateGamingLed(uint32_t now) {
  if (!configPortal.config().gamingRgb) {
    return;
  }
  if (static_cast<int32_t>(now - gamingLedHoldUntil) < 0) {
    return; // ステータス色を保持中
  }
  if (now - gamingLedLastUpdateAt < 40) {
    return; // 約25fpsにthrottle
  }
  gamingLedLastUpdateAt = now;

  uint8_t r, g, b;
  // LEDは眩しすぎないよう明度を抑える（V=0.3）。色相は顔と同期。
  hsvToRgb888(avatarFace.gamingHue(), 1.0f, 0.30f, r, g, b);
  M5StackChan.showRgbColor(r, g, b);
}

// 首を左右に振る喜びモーションを始める（サーボ校正済み・LEVEL HOLD 以外のとき）
void startJoyMotion() {
  if (!calibrationController.data().servoValid ||
      !modeUsesBodyMotion() ||
      levelHoldActive) {
    return;
  }
  if (bodyMotionState == BodyMotionState::Stopped &&
      !startBodyMotion(true)) {
    return;
  }

  bodyJoyCenterYaw = bodyHomeYaw();
  bodyJoyCenterPitch = bodyHomePitch();
  bodyMotionYawTarget = bodyJoyCenterYaw;
  bodyMotionPitchTarget = bodyJoyCenterPitch;
  bodyMotionStartedAt = millis();
  bodyMotionLastUpdateAt = 0;
  bodyMotionState = BodyMotionState::Joy;
}

void triggerPetHappyMotion() {
  if (avatarFace.isShowcaseEnabled()) avatarFace.toggleShowcase();
  if (printerScreenOpen) {
    petRestorePrinterScreen = true;
    closePrinterScreen();
  }
  avatarFace.setExpression(m5avatar::Expression::Happy);
  if (!petSpeaking) {
    avatarFace.showCaption(petReaction.level() >= 3 ? "だいすき！" : "なでなで、うれしい！", 2200);
  }
  avatarFace.returnToDefaultAfter(petReaction.remaining(millis()) + 800);
  showStatusLed(96, 24, 72, petReaction.remaining(millis()));
  if (calibrationController.data().servoValid && modeUsesBodyMotion() &&
      !levelHoldActive &&
      (bodyMotionState != BodyMotionState::Stopped || startBodyMotion(true))) {
    // Keep the current target and feedback-sync policy when excitement rises.
    bodyMotionState = BodyMotionState::Pet;
  }
  Serial.printf("[pet] stroke level=%u motion=%d\n", petReaction.level(),
                bodyMotionState == BodyMotionState::Pet);
}

// Sample every service cycle, including audio callbacks. Never start speech here.
void updatePetInteraction() {
  const uint32_t now = millis();
  const bool enabled = petInteractionReady && !configPortal.isPortalActive() &&
                       !modeMenuOpen && !settingsInfoOpen && !deviceSetupOpen &&
                       !usb_spectrum::isOpen();
  const auto& touch = M5StackChan.TouchSensor;
  const auto& strengths = touch.getIntensities();
  const uint8_t zones = (strengths[0] ? 1 : 0) | (strengths[1] ? 2 : 0) |
                        (strengths[2] ? 4 : 0);
  // wasSwiped() is not const in BSP 1.1.0.
  if (petReaction.update(now, zones, M5StackChan.TouchSensor.wasSwiped(), enabled)) {
    petFeedbackPending = true;
  }
  if (!enabled) {
    petFeedbackPending = petRestorePrinterScreen = false;
    return;
  }
  if (petFeedbackPending && (!speaking || petSpeaking)) {
    petFeedbackPending = false;
    if (petReaction.active(now)) triggerPetHappyMotion();
  }
  if (petRestorePrinterScreen && !petReaction.active(now) && !speaking) {
    petRestorePrinterScreen = false;
    if (currentMode == AppMode::Printer) openPrinterScreen();
  }
}

void updateBodyMotion() {
  if (bodyMotionState == BodyMotionState::Stopped) {
    if (!bodyMotionSkipAutoStart && bodyMotionCanUseServo()) {
      startBodyMotion();
    }
    return;
  }
  if (!bodyMotionCanUseServo()) {
    return;
  }

  const uint32_t now = millis();
  if (now - bodyMotionLastUpdateAt < BODY_MOTION_UPDATE_INTERVAL_MS) {
    return;
  }
  bodyMotionLastUpdateAt = now;

  if (bodyMotionState == BodyMotionState::Pet) {
    if (petReaction.active(now)) {
      const auto pose = petReaction.pose(now);
      const int yaw = clampBodyYaw(bodyHomeYaw() + pose.yaw);
      const int pitch = clampBodyPitch(bodyHomePitch() + pose.pitch);
      bodyMotionYawTarget = moveTowardByStep(bodyMotionYawTarget, yaw, 48);
      bodyMotionPitchTarget = moveTowardByStep(bodyMotionPitchTarget, pitch, 30);
      M5StackChan.Motion.move(bodyMotionYawTarget, bodyMotionPitchTarget, 900);
      return;
    }
    bodyMotionYawBase = bodyHomeYaw();
    bodyMotionPitchBase = bodyHomePitch();
    bodyMotionStartedAt = now;
    bodyMotionState = BodyMotionState::Idle;
    // Fall through with the previous targets intact for a smooth return.
  }

  if (bodyMotionState == BodyMotionState::Joy) {
    const uint32_t elapsed = now - bodyMotionStartedAt;
    if (elapsed >= BODY_JOY_DURATION_MS) {
      bodyMotionYawBase = bodyHomeYaw();
      bodyMotionPitchBase = bodyHomePitch();
      bodyMotionYawTarget = bodyMotionYawBase;
      bodyMotionPitchTarget = bodyMotionPitchBase;
      bodyMotionStartedAt = now;
      bodyMotionState = BodyMotionState::Idle;
      M5StackChan.Motion.move(
          bodyMotionYawTarget, bodyMotionPitchTarget,
          BODY_IDLE_SERVO_SPEED);
      showStatusLed(0, 48, 0);
      return;
    }

    const float t = elapsed / static_cast<float>(BODY_JOY_DURATION_MS);
    const float envelope = sinf(PI * t);
    const float swing =
        sinf(2.0f * PI * BODY_JOY_CYCLES * t) * envelope;
    const int yaw = clampBodyYaw(
        bodyJoyCenterYaw +
        static_cast<int>(roundf(BODY_JOY_YAW_AMPLITUDE * swing)));
    M5StackChan.Motion.move(yaw, bodyJoyCenterPitch, BODY_JOY_SERVO_SPEED);
    bodyMotionYawTarget = yaw;
    bodyMotionPitchTarget = bodyJoyCenterPitch;
    return;
  }

  const float elapsed = (now - bodyMotionStartedAt) / 1000.0f;
  int desiredYaw =
      bodyMotionYawBase +
      static_cast<int>(roundf(BODY_IDLE_YAW_AMPLITUDE *
                              sinf(elapsed * 2.1f)));
  int desiredPitch =
      bodyMotionPitchBase +
      static_cast<int>(roundf(BODY_IDLE_PITCH_AMPLITUDE *
                              sinf(elapsed * 1.45f + 1.2f)));

  desiredYaw = clampBodyYaw(desiredYaw);
  desiredPitch = clampBodyPitch(desiredPitch);
  bodyMotionYawTarget = moveTowardByStep(
      bodyMotionYawTarget, desiredYaw, BODY_MOTION_MAX_STEP);
  bodyMotionPitchTarget = moveTowardByStep(
      bodyMotionPitchTarget, desiredPitch, BODY_MOTION_MAX_STEP);
  M5StackChan.Motion.move(
      bodyMotionYawTarget, bodyMotionPitchTarget, BODY_IDLE_SERVO_SPEED);
}

// LEVEL HOLDモードを開始する。
// キャリブレーションが未完了またはIMUが使えない場合はエラーを表示してfalseを返す。
bool startLevelHoldMode() {
  const auto& calibration = calibrationController.data();
  if (!calibration.servoValid || !calibration.imuLevelValid ||
      !M5.Imu.isEnabled()) {
    avatarFace.setExpression(m5avatar::Expression::Doubt);
    avatarFace.showStatus("CAL REQUIRED", 2500);
    avatarFace.returnToDefaultAfter(2500);
    showStatusLed(96, 48, 0); // 橙色LED: キャリブレーション必要
    return false;
  }

  stopBodyMotion(false);

  // サーボ電源をONにしてトルクを有効化、ホーム位置に移動する
  M5StackChan.setServoPowerEnabled(true);
  delay(150);
  M5StackChan.Motion.setAutoAngleSyncEnabled(true);
  M5StackChan.Motion.setAutoTorqueReleaseEnabled(false);
  M5StackChan.Motion.setTorqueEnabled(true);
  delay(80);
  M5StackChan.Motion.move(0, 0, LEVEL_HOLD_SERVO_SPEED); // センターへ移動
  delay(120);
  M5StackChan.Motion.setAutoAngleSyncEnabled(false);
  resetLevelHoldPid();
  levelHoldActive = true;
  currentMode = AppMode::LevelHold;
  avatarFace.resetToDefault();
  avatarFace.showStatus("LEVEL HOLD", 1800);
  showStatusLed(0, 64, 96); // 水色LED: LEVEL HOLD動作中
  Serial.println("Mode changed: LEVEL HOLD");
  return true;
}

// LEVEL HOLDモードを停止する。サーボのトルクを切って電源をOFFにする。
void stopLevelHoldMode() {
  if (!levelHoldActive) {
    return;
  }
  M5StackChan.Motion.stop();
  delay(50);
  M5StackChan.Motion.setTorqueEnabled(false);
  M5StackChan.Motion.setAutoAngleSyncEnabled(true);
  M5StackChan.Motion.setAutoTorqueReleaseEnabled(true);
  M5StackChan.setServoPowerEnabled(false);
  levelHoldActive = false;
}

// 指定モードに切り替える
void activateMode(AppMode mode) {
  if (usb_spectrum::isOpen()) {
    usb_spectrum::close();
    avatarFace.resumeDrawing();
  }
  if (mode == AppMode::LevelHold) {
    // 校正が無いなどで始められなければ今のモードのまま
    if (startLevelHoldMode()) {
      closePrinterScreen();
      updatePrinterHud(printerNow);  // HUD を消す
    }
    return;
  }

  stopLevelHoldMode();
  if (mode != AppMode::Printer) closePrinterScreen();  // プリンター画面は MQTT モードのもの
  currentMode = mode;
  avatarFace.resetToDefault();
  updatePrinterHud(printerNow);  // MQTT モードの間だけ HUD を出す
  if (mode == AppMode::Printer) {
    avatarFace.showStatus("MQTT MODE", 1800);
    showStatusLed(0, 36, 72); // 水色LED: プリンター実況
  } else {
    avatarFace.showStatus("LOCAL LLM", 1800);
    showStatusLed(0, 48, 0); // 緑色LED: 通常動作
  }
  startBodyMotion();
  Serial.printf("Mode changed: %s\n", appModeName(mode));
}

// LEVEL HOLDモードのPID制御メインループ。80ms間隔で実行する。
//
// アルゴリズム:
//   1. IMU加速度を取得
//   2. ローパスフィルタで高周波ノイズを除去（alpha=0.35）
//   3. ロール・ピッチ誤差にデッドバンドを適用（±0.6度以下はゼロに）
//   4. PIDコントローラで補正角度を計算
//   5. レートリミッタで1フレーム最大5度に制限してサーボへ送る
void updateLevelHoldMode() {
  if (!levelHoldActive) {
    return;
  }

  const uint32_t now = millis();
  if (now - levelHoldLastUpdateAt < LEVEL_HOLD_UPDATE_INTERVAL_MS) {
    return; // まだ更新タイミングではない
  }
  const float dt = (now - levelHoldLastUpdateAt) / 1000.0f; // 経過時間（秒）
  levelHoldLastUpdateAt = now;

  if (M5.Imu.update() == m5::IMU_Class::sensor_mask_none) {
    return; // IMUのデータが更新されていない場合はスキップ
  }

  float ax = 0.0f;
  float ay = 0.0f;
  float az = 0.0f;
  M5.Imu.getAccel(&ax, &ay, &az);

  // 加速度ベクトルの大きさが小さすぎる（自由落下状態など）場合はスキップ
  const float accelNorm = sqrtf(ax * ax + ay * ay + az * az);
  if (accelNorm < LEVEL_HOLD_ACCEL_MIN_NORM) {
    return;
  }

  const auto& calibration = calibrationController.data();
  // キャリブレーション時のゼロ点を引いて水平からの偏差を求める
  const float rollDeg =
      atan2f(ay, az) * 180.0f / PI - calibration.levelRollDeg;
  const float pitchDeg =
      atan2f(-ax, sqrtf(ay * ay + az * az)) * 180.0f / PI -
      calibration.levelPitchDeg;

  // 一次ローパスフィルタ: filteredVal += (newVal - filteredVal) * alpha
  // alpha=0.35 → 高周波ノイズを抑えつつ応答性を維持
  if (!levelHoldFilterReady) {
    levelHoldFilteredRollDeg = rollDeg;   // 初回は即時設定
    levelHoldFilteredPitchDeg = pitchDeg;
    levelHoldFilterReady = true;
  } else {
    levelHoldFilteredRollDeg +=
        (rollDeg - levelHoldFilteredRollDeg) * LEVEL_HOLD_FILTER_ALPHA;
    levelHoldFilteredPitchDeg +=
        (pitchDeg - levelHoldFilteredPitchDeg) * LEVEL_HOLD_FILTER_ALPHA;
  }

  // デッドバンド適用: ±0.6度以内の微小誤差は無視（サーボのジッタ防止）
  const float rollError =
      applyDeadband(levelHoldFilteredRollDeg, LEVEL_HOLD_DEADBAND_DEG);
  const float pitchError =
      applyDeadband(levelHoldFilteredPitchDeg, LEVEL_HOLD_DEADBAND_DEG);

  // PID計算: rollはヨー軸、pitchはピッチ軸に対応（符号注意）
  const int yawCorrection = static_cast<int>(roundf(constrain(
      levelHoldRollPid.update(rollError, dt),
      -LEVEL_HOLD_MAX_OUTPUT_DEG,
      LEVEL_HOLD_MAX_OUTPUT_DEG)));
  const int pitchCorrection = static_cast<int>(roundf(constrain(
      -levelHoldPitchPid.update(pitchError, dt), // ピッチは符号が逆
      -LEVEL_HOLD_MAX_OUTPUT_DEG,
      LEVEL_HOLD_MAX_OUTPUT_DEG)));

  // キャリブレーションで記録した可動範囲内にクランプする
  const int yawTarget = constrain(
      yawCorrection, calibration.yawMin, calibration.yawMax);
  const int pitchTarget = constrain(
      pitchCorrection, calibration.pitchMin, calibration.pitchMax);

  // レートリミッタ: 1フレームで最大5度しか動かさない（急激な動きを防ぐ）
  levelHoldYawTarget = moveTowardByStep(
      levelHoldYawTarget, yawTarget, LEVEL_HOLD_MAX_STEP_DEG);
  levelHoldPitchTarget = moveTowardByStep(
      levelHoldPitchTarget, pitchTarget, LEVEL_HOLD_MAX_STEP_DEG);

  M5StackChan.Motion.move(
      levelHoldYawTarget, levelHoldPitchTarget, LEVEL_HOLD_SERVO_SPEED);
}

// 現在アクティブなモードの更新処理を呼ぶ
void updateActiveMode() {
  updateLevelHoldMode();
  updateBodyMotion();
}

// TTS再生中の口パク入力を受け取る（VoiceVoxClientから呼ばれる）
// level: PCMサンプルの平均振幅から算出した0〜100の口の開き度合い
// 表情を大げさに見せるため、開き具合を 1.5 倍に増幅してから適用する。
void setLipSyncLevel(int level) {
  targetMouthOpen = constrain(static_cast<int>(level * 1.5f), 0, 100);
  lipSyncLastInputAt = millis();
  lipSyncActive = true;
}

// 音声再生終了時に口を閉じるトリガー（VoiceVoxClientから呼ばれる）
void stopLipSync() {
  targetMouthOpen = 0;
  lipSyncActive = true;
  lipSyncLastInputAt = millis();
}

// 口パクアニメーションを更新する（30ms間隔）。
// 目標値に向かってスムーズに加速・減速させる（線形補間より自然な動き）。
void updateLipSync() {
  const uint32_t now = millis();

  // 最後の入力から220ms経過したら口を閉じる（無音区間の検出）
  if (lipSyncActive && now - lipSyncLastInputAt >= LIP_INPUT_TIMEOUT_MS) {
    targetMouthOpen = 0;
  }

  if (!lipSyncActive ||
      now - lipSyncLastFrameAt < LIP_FRAME_INTERVAL_MS) {
    return;
  }
  lipSyncLastFrameAt = now;

  // 開くときは素早く（残り距離の半分）、閉じるときはゆっくり（残り距離の1/4）
  if (currentMouthOpen < targetMouthOpen) {
    currentMouthOpen +=
        max(3, (targetMouthOpen - currentMouthOpen + 1) / 2);
    currentMouthOpen = min(currentMouthOpen, targetMouthOpen);
  } else if (currentMouthOpen > targetMouthOpen) {
    currentMouthOpen -=
        max(2, (currentMouthOpen - targetMouthOpen + 3) / 4);
    currentMouthOpen = max(currentMouthOpen, targetMouthOpen);
  }

  avatarFace.setMouthOpenRatio(currentMouthOpen / 100.0f);

  // 口が完全に閉じたら口パクを停止する
  if (currentMouthOpen == 0 && targetMouthOpen == 0) {
    lipSyncActive = false;
  }
}

// メインループで毎フレーム呼ぶサービス関数。
// 全サブシステムの更新処理をまとめて実行する。
// TTS再生中もこれを呼ぶことで、Webサーバの応答などを継続する。
void serviceApp() {
  M5StackChan.update();    // タッチセンサ・ボタン・LED等の更新
  updatePetInteraction();  // 撫でる入力は発話中にも取得（発話自体はloopで）
  configPortal.update();   // Webサーバのリクエスト処理
  usb_audio::setSpeechActive(speaking);
  const bool homeVisible = currentMode == AppMode::LocalLlm &&
      !modeMenuOpen && !settingsInfoOpen && !deviceSetupOpen &&
      !printerScreenOpen && !usb_spectrum::isOpen() && !configPortal.isPortalActive();
  usb_spectrum::update(homeVisible);
  uint8_t spectrumBars[32] = {};
  usb_spectrum::copyBars(spectrumBars);
  avatarFace.hud().setSpectrum(homeVisible && usb_spectrum::isActive(), spectrumBars);
  avatarFace.update();     // アバターのステータス・まばたき・ショーケース更新
  updateLipSync();         // 口パクアニメーション更新
  updateActiveMode();      // LEVEL HOLDなどのモード固有処理
  updatePrinterMonitor(millis()); // プリンタ状態の取り込み・実況判定・HUD 更新
  // 本体LED: 印刷中は進捗表示、それ以外はゲーミングRGB（虹色循環）
  if (!updatePrinterLed(millis())) {
    updateGamingLed(millis());
  }

  // 空きヒープの最小値を毎フレーム追跡する（断片化・リーク検出用）
  const uint32_t freeHeapNow = ESP.getFreeHeap();
  if (freeHeapNow < minFreeHeapEver) {
    minFreeHeapEver = freeHeapNow;
  }

  // /statusページ用のランタイム状態を2秒ごとに更新する
  static uint32_t lastStatusUpdateAt = 0;
  const uint32_t now = millis();
  if (now - lastStatusUpdateAt >= 2000) {
    RuntimeStatus status;
    status.appMode = appModeName(currentMode);
    status.servoCalibrated = calibrationController.data().servoValid;
    status.imuCalibrated = calibrationController.data().imuLevelValid;
    status.resetReason = bootResetReason;
    status.freeHeap = freeHeapNow;
    status.minFreeHeap = minFreeHeapEver;
    status.maxAllocHeap = ESP.getMaxAllocHeap();
    status.freePsram = ESP.getFreePsram();
    status.uiFrames = printerScreenRenderer.frames();
    status.uiPixels = printerScreenRenderer.pixels();
    status.cameraActive = cameraGazeActive;
    status.voiceClips = builtinVoice.isReady() ? builtinVoice.clipCount() : 0;
    status.voiceFreeText = builtinVoice.canReadAnything();
    configPortal.setRuntimeStatus(status);
    lastStatusUpdateAt = now;
  }

  // 15秒ごとにヒープ状況をシリアルへ出力する（クラッシュ前の推移を確認するため）。
  // free が徐々に減る=リーク、free に対し max-alloc が極端に小さい=断片化。
  static uint32_t lastHeapLogAt = 0;
  if (now - lastHeapLogAt >= 15000) {
    // スタックの残り（最も少なかったとき）: 顔の描画・メインループ・MQTT
    Serial.printf("[heap] free=%u min=%u maxblk=%u psram=%u stack face=%u loop=%u mqtt=%u\n",
                  (unsigned)freeHeapNow, (unsigned)minFreeHeapEver,
                  (unsigned)ESP.getMaxAllocHeap(),
                  (unsigned)ESP.getFreePsram(),
                  (unsigned)avatarFace.drawStackFree(),
                  (unsigned)uxTaskGetStackHighWaterMark(nullptr),
                  (unsigned)bambu.stackFree());
    lastHeapLogAt = now;
  }
}

// 設定中のTTSサーバでテキストを再生する（演出なしの共通処理・ブロッキング）。
bool playTts(const String& text) {
  const AppConfig& appConfig = configPortal.config();
  TtsConfig ttsConfig;
  ttsConfig.host = appConfig.ttsHost;
  ttsConfig.port = appConfig.ttsPort;
  ttsConfig.speaker = appConfig.ttsSpeaker;
  ttsConfig.engineType =
      ttsEngineTypeFromString(appConfig.ttsEngineType);

  const bool success = ttsClient.speak(ttsConfig, text);
  stopLipSync(); // 再生終了後に口を閉じる
  return success;
}

// 実況などの文章を喋る。
//   - エンジンが内蔵ボイスなら内蔵ボイスで喋る
//   - サーバー指定ならサーバーで喋り、失敗したら内蔵ボイスで言い直す。
//     失敗後の3分間はサーバーを試さず（接続待ちで固まらないよう）内蔵ボイスを使う
bool speakSentence(const String& text) {
  if (configPortal.config().ttsEngineType == "builtin") {
    return builtinVoice.speak(text);
  }
  const uint32_t now = millis();
  const bool serverResting =
      ttsServerDownAt != 0 && now - ttsServerDownAt < kTtsServerRetryMs;
  if (configPortal.isConnected() && (!serverResting || !builtinVoice.isReady())) {
    if (playTts(ttsTextFor(text))) {
      ttsServerDownAt = 0;
      return true;
    }
    ttsServerDownAt = now == 0 ? 1 : now;
    Serial.printf("TTS server failed (%s), using builtin voice\n",
                  ttsClient.lastError().c_str());
  }
  return builtinVoice.speak(text);
}

// 指定テキストをTTSで読み上げる（共通処理）。
// 話し中・WiFi未接続・ショーケース中の場合は早期リターンする。
// simple_wav エンジンではこの text がそのまま Gateway の /synthesis に POST される。
//   - 設定テキスト "__REASK_LAST__"（Aボタン）→ Gateway が最後の質問を再LLM処理
//   - "__CURRENT__"（/api/speak 既定）→ Gateway が current.wav を返す
void speakText(const String& text) {
  // サーバーを使うのは「サーバー指定かつ Wi-Fi 接続中」のときだけ。
  // それ以外（内蔵ボイス指定・Wi-Fi なし）は内蔵ボイスで喋る。
  const bool useServer = configPortal.config().ttsEngineType != "builtin" &&
                         configPortal.isConnected();
  if (speaking || (!useServer && !builtinVoice.isReady())) {
    return;
  }

  // ショーケースモード中は一旦停止する
  if (avatarFace.isShowcaseEnabled()) {
    avatarFace.toggleShowcase();
  }

  speaking = true;
  avatarFace.setExpression(m5avatar::Expression::Happy);
  String caption = text;
  if (caption.startsWith("__SAY__")) caption = caption.substring(7);
  else if (caption.startsWith("__")) caption = "";  // Gatewayの音声だけでは本文が不明
  avatarFace.showCaption(caption, 0);
  showStatusLed(0, 0, 96); // 青色LED: TTS通信中

  bool success = useServer && playTts(text);
  // サーバーが使えない・失敗したら内蔵ボイスで。Gateway 用の指示（__CURRENT__・
  // __REASK_LAST__）や読めない文章はあいさつに置き換える（__SAY__ は本文を読む）。
  if (!success && builtinVoice.isReady()) {
    String spoken = text;
    if (spoken.startsWith("__SAY__")) {
      spoken = spoken.substring(7);
    } else if (spoken.startsWith("__")) {
      spoken = kBuiltinGreeting;
    }
    caption = spoken;
    avatarFace.showCaption(caption, 0);
    success = builtinVoice.speak(spoken);
    if (!success) {
      caption = kBuiltinGreeting;
      avatarFace.showCaption(caption, 0);
      success = builtinVoice.speak(caption);
    }
  }

  if (success) {
    avatarFace.resetToDefault();
    showStatusLed(0, 48, 0); // 緑色LED: 正常
  } else {
    avatarFace.setExpression(m5avatar::Expression::Angry);
    avatarFace.showStatus("TTS ERROR", 2500);
    avatarFace.returnToDefaultAfter(2500);
    showStatusLed(96, 0, 0, 3000); // 赤色LED: エラー（3秒保持）
    Serial.printf("TTS error: %s\n", ttsClient.lastError().c_str());
  }

  speaking = false;
  if (success) avatarFace.showCaption(caption, 2500);
  else avatarFace.hud().clearCaption();
}

// 起動時のあいさつ（内蔵ボイスがあるときだけ。スピーカーの動作確認も兼ねる）
void greetOnBoot() {
  if (!builtinVoice.isReady() || !configPortal.config().commentaryVoice) {
    return;
  }
  speaking = true;
  avatarFace.setExpression(m5avatar::Expression::Happy);
  avatarFace.showCaption(kBuiltinGreeting, 0);
  builtinVoice.speak(kBuiltinGreeting);
  speaking = false;
  avatarFace.showCaption(kBuiltinGreeting, 2500);
  avatarFace.returnToDefaultAfter(1500);
}

// 設定された「Text to speak」をTTSで読み上げる（Aボタン・頭タッチ用）。
void speakConfiguredText() {
  speakText(configPortal.config().speechText);
}

// ---------------------------------------------------------------------------
// プリンタ監視・実況
// ---------------------------------------------------------------------------

// 実況テキストを TTS に渡す形へ変換する。
// simple_wav（Android Gateway）は本文をそのまま送ると current.wav を返す仕様なので、
// "__SAY__" 接頭辞で「この文章を読み上げて」と伝える（Gateway 側で対応済み）。
String ttsTextFor(const String& text) {
  if (configPortal.config().ttsEngineType == "simple_wav") {
    return "__SAY__" + text;
  }
  return text;
}

m5avatar::Expression expressionFor(CommentMood mood) {
  switch (mood) {
    case CommentMood::Happy:  return m5avatar::Expression::Happy;
    case CommentMood::Sad:    return m5avatar::Expression::Sad;
    case CommentMood::Doubt:  return m5avatar::Expression::Doubt;
    case CommentMood::Angry:  return m5avatar::Expression::Angry;
    case CommentMood::Sleepy: return m5avatar::Expression::Sleepy;
    default:                  return m5avatar::Expression::Neutral;
  }
}

// 実況の気分に合わせて LED を一瞬光らせる
void showMoodLed(CommentMood mood) {
  switch (mood) {
    case CommentMood::Happy:  showStatusLed(0, 72, 24, 2500); break;
    case CommentMood::Sad:    showStatusLed(0, 12, 72, 2500); break;
    case CommentMood::Doubt:  showStatusLed(72, 44, 0, 2500); break;
    case CommentMood::Angry:  showStatusLed(96, 0, 0, 3000); break;
    case CommentMood::Sleepy: showStatusLed(24, 0, 48, 2500); break;
    default:                  showStatusLed(0, 36, 56, 2500); break;
  }
}

// 夜間（設定の開始〜終了時刻）かどうか。時刻が未同期なら夜間扱いにしない。
bool inQuietHours() {
  const AppConfig& c = configPortal.config();
  if (!c.quietEnabled || c.quietFrom == c.quietTo) return false;
  const time_t now = time(nullptr);
  if (now < 1700000000) return false;
  struct tm t;
  localtime_r(&now, &t);
  const int h = t.tm_hour;
  if (c.quietFrom < c.quietTo) {
    return h >= c.quietFrom && h < c.quietTo;
  }
  return h >= c.quietFrom || h < c.quietTo;  // 日付をまたぐ（例: 23時〜7時）
}

// 字幕だけ出すときの表示時間（文字数からおおよその読了時間を見積もる）
uint32_t captionDurationMs(const String& text) {
  const uint32_t chars = text.length() / 3;  // UTF-8 の日本語は1文字3バイト
  return constrain(2500UL + chars * 180UL, 4000UL, 20000UL);
}

void drawPrinterScreenNow() {
  auto& canvas = startupCanvas();
  printerScreenRenderer.draw(canvas, printerNow, lastCommentText,
                             configPortal.config().commentaryVoice);
  printerScreenDrawnAt = millis();
  printerScreenRevision = printerRevisionSeen;
}

void openPrinterScreen() {
  if (printerScreenOpen || usb_spectrum::isOpen()) return;
  printerScreenOpen = true;
  avatarFace.pauseDrawing();
  printerScreenRenderer.invalidate();
  drawPrinterScreenNow();
}

void closePrinterScreen() {
  if (!printerScreenOpen) return;
  printerScreenOpen = false;
  avatarFace.resumeDrawing();
}

// 状態が変わったとき・1秒ごとに本体のプリンタ詳細画面を描き直す
void refreshPrinterScreen() {
  if (!printerScreenOpen) return;
  if (millis() - printerScreenDrawnAt < 200) return;  // 受信集中時も最大5fps
  if (printerRevisionSeen != printerScreenRevision ||
      millis() - printerScreenDrawnAt >= 1000) {
    drawPrinterScreenNow();
  }
}

// 実況コメントを演出付きで再生する（字幕・表情・LED・声・喜びモーション）。
// ブロッキング（TTS 再生中も serviceApp() が回り続ける）。
void performComment(Comment comment) {
  if (comment.text.isEmpty() || speaking) return;
  if (comment.priority == CommentPriority::High) {
    petReaction.cancel();  // 完了・エラーなどの通知を優先する
    petFeedbackPending = false;
  }

  comment.createdAt = millis();
  commentator.remember(comment);
  lastCommentText = comment.text;
  Serial.printf("[commentary] %s\n", comment.text.c_str());

  if (avatarFace.isShowcaseEnabled()) {
    avatarFace.toggleShowcase();
  }
  avatarFace.setExpression(expressionFor(comment.mood));
  showMoodLed(comment.mood);
  if (printerScreenOpen) {
    drawPrinterScreenNow();
  }

  // 夜間は High（完了・失敗・エラー・手動の依頼）だけ声に出し、ほかは字幕のみ。
  // 内蔵ボイスがあれば Wi-Fi やサーバーが無くても喋れる。
  const bool canSpeak = builtinVoice.isReady() || configPortal.isConnected();
  const bool voice =
      configPortal.config().commentaryVoice && canSpeak &&
      !(inQuietHours() && comment.priority != CommentPriority::High);
  uint32_t holdMs = captionDurationMs(comment.text);
  if (voice) {
    avatarFace.showCaption(comment.text, 0);  // 喋り終わるまで出し続ける
    speaking = true;
    const bool ok = speakSentence(comment.spoken());
    speaking = false;
    holdMs = 2500;
    avatarFace.showCaption(comment.text, holdMs);
    if (!ok) {
      avatarFace.showStatus("TTS ERROR", 2500);
      showStatusLed(96, 0, 0, 3000);
      Serial.printf("TTS error: %s / builtin: %s\n",
                    ttsClient.lastError().c_str(),
                    builtinVoice.lastError().c_str());
    }
  } else {
    avatarFace.showCaption(comment.text, holdMs);
  }

  if (comment.celebrate) {
    startJoyMotion();
    showStatusLed(0, 96, 32, 6000);
  }
  avatarFace.returnToDefaultAfter(holdMs + 500);
  commentBusyUntil = millis() + (voice ? kCommentGapMs : holdMs);
}

// A short, direct reply to petting. Prefer onboard speech so a sleeping TTS
// server does not delay the reaction; continue sampling touches during playback.
bool performPetResponse() {
  if (speaking || petFeedbackPending || !petReaction.takeSpeech(millis())) return false;
  static const char* const gentle[] = {
      "えへへ、なでなでうれしい！", "わあ、ありがとう！もっとなでて！",
      "なでなで、だいすき！", "きもちいいなあ。ありがとう！"};
  static const char* const excited[] = {
      "わあい！うれしくてたまらないよ！", "えへへ、だいすき！ずっといっしょだよ！",
      "もっともっと、なでなでして！", "しあわせいっぱい！ありがとう！"};
  static uint8_t reply = 0;
  const uint8_t level = petReaction.level();
  const String text = (level >= 3 ? excited : gentle)[reply++ % 4];
  Comment comment;
  comment.text = text;
  comment.mood = CommentMood::Happy;
  comment.createdAt = millis();
  commentator.remember(comment);
  lastCommentText = text;
  avatarFace.setExpression(m5avatar::Expression::Happy);
  const bool voice = configPortal.config().commentaryVoice &&
                     (builtinVoice.isReady() || configPortal.isConnected());
  avatarFace.showCaption(text, voice ? 0 : 6000);
  bool ok = true;
  if (voice) {
    petSpeaking = speaking = true;
    ok = builtinVoice.isReady() ? builtinVoice.speak(text) : speakSentence(text);
    stopLipSync();
    speaking = petSpeaking = false;
    avatarFace.showCaption(text, 2500);
  }
  avatarFace.returnToDefaultAfter(petReaction.remaining(millis()) + 800);
  commentBusyUntil = millis() + kCommentGapMs;
  Serial.printf("[pet] reply level=%u voice=%d ok=%d\n", level, voice, ok);
  return true;
}

// 頭タップ: いまの状況をまとめて話す
void reportPrinterStatus() {
  Comment report = commentator.statusReport(bambu.snapshot());
  report.priority = CommentPriority::High;  // 直接たずねられたので夜間でも答える
  performComment(report);
}

// HUD の表示内容をプリンタ状態から作る
void updatePrinterHud(const PrinterState& s) {
  const bool visible = configPortal.config().printerHud && bambu.isEnabled() &&
                       currentMode == AppMode::Printer;
  avatarFace.setHudVisible(visible);
  if (!visible) {
    return;
  }

  HudData d;
  d.visible = true;
  const bool online = s.link == LinkState::Online && s.synced;
  const char* phase = printPhaseLabelJa(s.phase);
  if (!online) {
    switch (s.link) {
      case LinkState::Online:      phase = "取得中…"; break;
      case LinkState::Error:       phase = "接続エラー"; break;
      case LinkState::WaitingWifi: phase = "Wi-Fi待ち"; break;
      default:                     phase = "接続中…"; break;
    }
  }
  strlcpy(d.phase, phase, sizeof(d.phase));
  if (online) {
    d.active = s.isActive() || s.phase == PrintPhase::Finish;
    d.alert = s.hmsCount > 0 || s.phase == PrintPhase::Pause ||
              (s.phase == PrintPhase::Failed && !isCancelError(s.printError));
    d.percent = s.phase == PrintPhase::Finish ? 100 : s.percent;
    if (s.isActive() && s.remainingMin >= 0) {
      strlcpy(d.remaining, remainingShort(s.remainingMin).c_str(),
              sizeof(d.remaining));
      strlcpy(d.eta, etaClockShort(s.remainingMin).c_str(), sizeof(d.eta));
    }
    strlcpy(d.job, s.jobName, sizeof(d.job));
    d.layer = s.layer;
    d.totalLayers = s.totalLayers;
    if (!isnan(s.nozzleTemp)) d.nozzle = static_cast<int>(lroundf(s.nozzleTemp));
    if (!isnan(s.bedTemp)) d.bed = static_cast<int>(lroundf(s.bedTemp));
  }
  avatarFace.hud().set(d);
}

// プリンタ状態を取り込み、実況判定と HUD 更新を行う（serviceApp から毎フレーム）。
// 状態が変わったとき、または1秒ごと（時刻・接続断の判定用）にだけ処理する。
void updatePrinterMonitor(uint32_t now) {
  if (!bambu.isEnabled()) {
    return;
  }
  const uint32_t revision = bambu.revision();
  if (revision == printerRevisionSeen && now - printerEvaluatedAt < 1000) {
    return;
  }
  printerRevisionSeen = revision;
  printerEvaluatedAt = now;
  printerNow = bambu.snapshot();
  if (printerNow.phase != printerLastPhase) {
    printerLastPhase = printerNow.phase;
    printerPhaseSince = now;
  }
  commentator.update(printerNow, now);
  updatePrinterHud(printerNow);
}

// 印刷中は本体 LED（左右6灯ずつ）で進捗を表示する。
// 戻り値: LED を使った=true（ゲーミングRGB は描かない）
bool updatePrinterLed(uint32_t now) {
  static bool ledInUse = false;
  const PrinterState& s = printerNow;
  const bool recent = now - printerPhaseSince < kPhaseLedHoldMs;
  const bool failed =
      s.phase == PrintPhase::Failed && !isCancelError(s.printError) && recent;
  const bool finished = s.phase == PrintPhase::Finish && recent;
  const bool wanted = configPortal.config().ledProgress && bambu.isEnabled() &&
                      s.link == LinkState::Online && s.synced &&
                      (s.isActive() || failed || finished);
  if (!wanted) {
    if (ledInUse) {
      // 進捗表示をやめたら通常表示へ戻す（ゲーミングRGB OFF 時は緑=待機に戻す）
      ledInUse = false;
      if (!configPortal.config().gamingRgb) {
        M5StackChan.showRgbColor(0, 48, 0);
      }
    }
    return false;
  }
  ledInUse = true;
  if (static_cast<int32_t>(now - gamingLedHoldUntil) < 0) {
    return true;  // イベント色を保持中
  }
  static uint32_t lastLedAt = 0;
  if (now - lastLedAt < 50) {
    return true;
  }
  lastLedAt = now;

  const float wave = 0.5f + 0.5f * sinf(now * 0.004f);
  if (failed) {
    const bool on = (now / 400) % 2 == 0;
    M5StackChan.showRgbColor(on ? 90 : 4, 0, 0);
  } else if (finished) {
    const uint8_t g = static_cast<uint8_t>(20 + 60 * wave);
    M5StackChan.showRgbColor(g / 3, g, g / 2);
  } else if (s.phase == PrintPhase::Pause) {
    const bool on = (now / 600) % 2 == 0;
    M5StackChan.showRgbColor(on ? 70 : 6, on ? 42 : 3, 0);
  } else if (s.phase == PrintPhase::Running) {
    // 左右それぞれ6灯で 0〜100% を表す。途中の1灯は明るさで端数を表し、ゆっくり脈打つ。
    const float level = constrain(s.percent, 0, 100) / 100.0f * 6.0f;
    for (int i = 0; i < 6; ++i) {
      const float f = constrain(level - i, 0.0f, 1.0f);
      uint8_t g = 3;
      uint8_t b = 1;
      if (f >= 1.0f) {
        g = 64;
        b = 12;
      } else if (f > 0.0f) {
        g = static_cast<uint8_t>(6 + 58 * f * (0.55f + 0.45f * wave));
        b = g / 5;
      }
      M5StackChan.setRgbColor(i, 0, g, b);
      M5StackChan.setRgbColor(6 + i, 0, g, b);
    }
    M5StackChan.refreshRgb();
  } else {
    // PREPARE / SLICING: 青くゆっくり呼吸
    const uint8_t b = static_cast<uint8_t>(6 + 56 * wave);
    M5StackChan.showRgbColor(0, b / 4, b);
  }
  return true;
}

// 手動でモードを選んだとき（メニュー・Web）。自動切り替えの記録を消し、
// 印刷中に MQTT モードから抜けたならこのジョブの間は自動で戻さない。
void selectModeManually(AppMode mode) {
  petReaction.cancel();
  petFeedbackPending = petRestorePrinterScreen = false;
  printerModeAuto = false;
  if (mode != AppMode::Printer && printerNow.isActive()) {
    autoModeHeldOff = true;
  }
  activateMode(mode);
}

// 印刷が始まったら MQTT モードへ、終わって5分たったら元のモードへ戻す（loop から）。
// 実況や TTS の途中（serviceApp の中）では切り替えない。
void updateAutoPrinterMode(uint32_t now) {
  if (!bambu.isEnabled() || speaking || petReaction.active(now) || usb_spectrum::isOpen()) return;
  const PrinterState& s = printerNow;
  if (!s.synced) return;

  // 進行中かどうかは最後に受け取った状態で判断する（接続が切れても印刷中なら留まる）
  if (s.isActive()) {
    printerIdleSince = 0;
    if (currentMode != AppMode::Printer && s.link == LinkState::Online &&
        configPortal.config().autoPrinterMode && !autoModeHeldOff) {
      const AppMode previous = currentMode;
      activateMode(AppMode::Printer);
      if (currentMode == AppMode::Printer) {
        printerModeAuto = true;
        modeBeforeAuto = previous;
        showPrinterScreenIfFree();  // 顔よりプリンターの画面を優先して出す
        Serial.printf("[mode] print started: %s -> MQTT\n", appModeName(previous));
      }
    }
    return;
  }

  if (s.link == LinkState::Online) {
    autoModeHeldOff = false;  // ジョブが終わったので次の印刷ではまた自動で入る
  }
  if (!printerModeAuto) return;
  if (currentMode != AppMode::Printer) {
    printerModeAuto = false;
    return;
  }
  if (printerIdleSince == 0) {
    printerIdleSince = now == 0 ? 1 : now;
    return;
  }
  if (now - printerIdleSince < kAutoModeReturnMs) return;
  printerModeAuto = false;
  Serial.printf("[mode] print over: MQTT -> %s\n", appModeName(modeBeforeAuto));
  activateMode(modeBeforeAuto);
}

// 頭タップ: MQTT モードなら状況報告、そうでなければ設定テキストを話す
void onHeadTap() {
  if (bambu.isEnabled() && currentMode == AppMode::Printer) {
    reportPrinterStatus();
  } else {
    speakConfiguredText();
  }
}

// 頭部タッチセンサの入力を処理する。
// 撫でる反応はserviceAppで処理。ダブルクリックで目パターン、3回クリックで変形、
// 長押しでショーケース、シングルクリックでTTS読み上げ。
void handleTopTouch() {
  auto& touch = M5StackChan.TouchSensor;

  // Stroking must not also become a click, eye change or showcase hold.
  if (petReaction.suppressesClicks(millis())) return;

  if (touch.wasDoubleClicked()) {
    avatarFace.nextEyePattern(); // ダブルクリック: 次の目パターン
  } else if (touch.wasSingleClicked()) {
    onHeadTap(); // シングルクリック: 状況報告 or TTS読み上げ
  } else if (touch.wasDecideClickCount() &&
             touch.getClickCount() >= 3) {
    avatarFace.nextTransform(); // 3回以上クリック: 次の変形パターン
  }

  if (touch.wasHold()) {
    avatarFace.toggleShowcase(); // 長押し: ショーケースモード切り替え
  }
}

// メニューのタイル配置（2列×4行、FFT・設定・閉じるを含む）。
// 設定（Wi-Fi・プリンター・接続情報）は「設定」の中にまとめる。描画と当たり判定で共有する。
constexpr ModeMenuButton kMenuOrder[] = {
    ModeMenuButton::PrinterMode, ModeMenuButton::LocalLlm,
    ModeMenuButton::LevelHold,   ModeMenuButton::Printer,
    ModeMenuButton::Voice,       ModeMenuButton::Fft,
    ModeMenuButton::Settings,
    ModeMenuButton::Close,
};
constexpr int16_t kMenuColumns = 2;
constexpr int16_t kMenuTileRows = 4;
constexpr int16_t kMenuTop = 34;
constexpr int16_t kMenuMargin = 8;
constexpr int16_t kMenuGap = 6;

bool modeMenuTileRect(size_t index, int16_t& x, int16_t& y, int16_t& w,
                      int16_t& h) {
  const size_t count = sizeof(kMenuOrder) / sizeof(kMenuOrder[0]);
  if (index >= count) return false;
  const int16_t width = M5.Display.width();
  const int16_t height = M5.Display.height();
  const int16_t area = height - kMenuTop - kMenuMargin;
  w = (width - kMenuMargin * 2 - kMenuGap * (kMenuColumns - 1)) / kMenuColumns;
  h = (area - kMenuGap * (kMenuTileRows - 1)) / kMenuTileRows;
  x = kMenuMargin + static_cast<int16_t>(index % kMenuColumns) * (w + kMenuGap);
  y = kMenuTop + static_cast<int16_t>(index / kMenuColumns) * (h + kMenuGap);
  return true;
}

// タッチ座標からメニューボタンを判定する
ModeMenuButton modeMenuButtonAt(int16_t x, int16_t y) {
  for (size_t i = 0; i < sizeof(kMenuOrder) / sizeof(kMenuOrder[0]); ++i) {
    int16_t tx, ty, tw, th;
    modeMenuTileRect(i, tx, ty, tw, th);
    if (x >= tx && x < tx + tw && y >= ty && y < ty + th) {
      return kMenuOrder[i];
    }
  }
  return ModeMenuButton::None;
}

// モード選択メニューを描画する（2列×4行）。
// 選択中のモードは黄色の枠、押下中のタイルはアクセント色で塗る。
void drawModeMenu(ModeMenuButton pressed) {
  auto& display = startupCanvas();
  const int16_t width = display.width();
  const uint16_t bg = setupui::kBg;
  const uint16_t card = setupui::kCard;
  const uint16_t sub = setupui::kSub;

  display.fillScreen(bg);
  display.setTextSize(1);
  display.setFont(&fonts::lgfxJapanGothicP_16);
  display.setTextDatum(middle_left);
  display.setTextColor(TFT_WHITE, bg);
  display.drawString("メニュー", 10, 17);
  display.setTextDatum(middle_right);
  if (modeMenuHintUntil != 0) {
    display.setTextColor(TFT_YELLOW, bg);
    display.drawString(modeMenuHint, width - 10, 17);
  } else {
    display.setTextColor(sub, bg);
    String modeNote = String("モード: ") + appModeName(currentMode);
    if (printerModeAuto) modeNote += "（自動）";
    display.drawString(modeNote, width - 10, 17);
  }

  const bool printerOn = bambu.isEnabled();
  const bool voiceOn = configPortal.config().commentaryVoice;
  for (size_t i = 0; i < sizeof(kMenuOrder) / sizeof(kMenuOrder[0]); ++i) {
    const ModeMenuButton button = kMenuOrder[i];
    int16_t x, y, w, h;
    modeMenuTileRect(i, x, y, w, h);

    const char* title = "";
    String note;
    uint16_t accent = sub;
    switch (button) {
      case ModeMenuButton::PrinterMode:
        title = "MQTT";
        note = !printerOn ? "未設定"
               : configPortal.config().autoPrinterMode ? "印刷で自動"
                                                       : "プリンター実況";
        accent = display.color565(250, 204, 21);
        break;
      case ModeMenuButton::Printer:
        title = "プリンター";
        note = printerOn ? String(printPhaseLabelJa(printerNow.phase))
                         : String("未設定");
        accent = display.color565(74, 222, 128);
        break;
      case ModeMenuButton::Voice:
        title = "実況の声";
        note = voiceOn ? "ON" : "OFF・字幕のみ";
        accent = display.color565(96, 165, 250);
        break;
      case ModeMenuButton::LocalLlm:
        title = "ホーム";
        note = "会話・USB音声";
        accent = display.color565(52, 211, 153);
        break;
      case ModeMenuButton::LevelHold:
        title = "LEVEL HOLD";
        note = "水平を保つ";
        accent = display.color565(56, 189, 248);
        break;
      case ModeMenuButton::Fft:
        title = "FFT";
        note = usb_spectrum::isReady() ? "USBスペクトラム" : "初期化エラー";
        accent = display.color565(71, 221, 201);
        break;
      case ModeMenuButton::Settings:
        title = "設定";
        note = "Wi-Fi・プリンター";
        accent = display.color565(192, 132, 252);
        break;
      default:
        title = "閉じる";
        note = "顔に戻る";
        accent = display.color565(139, 146, 163);
        break;
    }

    const bool selected =
        (button == ModeMenuButton::PrinterMode &&
         currentMode == AppMode::Printer) ||
        (button == ModeMenuButton::LocalLlm &&
         currentMode == AppMode::LocalLlm) ||
        (button == ModeMenuButton::LevelHold &&
         currentMode == AppMode::LevelHold);
    const bool isPressed = pressed == button;
    const uint16_t fill = isPressed ? accent : card;
    const uint16_t textColor = isPressed ? bg : TFT_WHITE;

    display.fillRoundRect(x, y, w, h, 9, fill);
    display.fillRoundRect(x, y + 10, 4, h - 20, 2, isPressed ? bg : accent);
    if (selected) {
      display.drawRoundRect(x, y, w, h, 9, TFT_YELLOW);
      display.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 8, TFT_YELLOW);
    }
    display.setFont(&fonts::lgfxJapanGothicP_16);
    display.setTextColor(textColor, fill);
    if (button == ModeMenuButton::Close) {
      display.setTextDatum(middle_center);
      display.drawString(title, x + w / 2, y + h / 2 + 1);
      continue;
    }
    display.setTextDatum(top_left);
    display.drawString(title, x + 10, y + 5);
    display.setFont(&fonts::lgfxJapanGothicP_12);
    display.setTextColor(isPressed ? bg : sub, fill);
    display.drawString(setupui::fitText(display, note, w - 14), x + 10, y + 26);
  }
  display.pushSprite(0, 0); // キャンバスを画面に転送（フリッカーフリー）
}

// SETTINGS選択時に表示する接続情報画面を描画する。
// Web ダッシュボード・設定画面の URL、設定用ホットスポット、プリンターの接続状態を出す。
void drawSettingsInfo() {
  auto& display = startupCanvas();
  const int16_t width = display.width();
  const int16_t height = display.height();
  const uint16_t bg = setupui::kBg;
  const uint16_t card = setupui::kCard;
  const uint16_t sub = setupui::kSub;
  const uint16_t green = display.color565(74, 222, 128);
  const uint16_t cyan = display.color565(96, 165, 250);

  display.fillScreen(bg);
  display.setTextSize(1);
  display.setFont(&fonts::lgfxJapanGothicP_16);
  display.setTextDatum(middle_left);
  display.setTextColor(TFT_WHITE, bg);
  display.drawString("設定・接続情報", 12, 18);

  int16_t y = 38;
  auto block = [&](const String& title, const String& line1, uint16_t color,
                   const String& line2) {
    const int16_t h = line2.isEmpty() ? 42 : 58;
    display.fillRoundRect(8, y, width - 16, h, 10, card);
    display.setTextDatum(top_left);
    display.setTextColor(sub, card);
    display.drawString(title, 18, y + 4);
    display.setTextColor(color, card);
    display.drawString(line1, 18, y + 22);
    if (!line2.isEmpty()) {
      display.setTextColor(sub, card);
      display.drawString(line2, 18, y + 40);
    }
    y += h + 6;
  };

  // Wi-Fi 接続中: ブラウザで開く URL
  if (configPortal.isConnected()) {
    const String base = "http://" + configPortal.localIp().toString();
    block("ブラウザで開く（同じ Wi-Fi）", base + "/", green,
          "設定: " + base + "/settings");
  }

  // 設定用ホットスポット（192.168.4.1）
  if (configPortal.isSettingsApActive()) {
    block("ホットスポット " + configPortal.accessPointName(),
          "http://" + configPortal.settingsApIp().toString() + "/", cyan,
          "パスワード: stackchan");
  }

  // プリンターの接続状態
  if (bambu.isEnabled()) {
    const PrinterState s = bambu.snapshot();
    String state;
    uint16_t color = sub;
    switch (s.link) {
      case LinkState::Online:
        state = String("接続中・") + printPhaseLabelJa(s.phase);
        color = green;
        break;
      case LinkState::Error:
        state = "接続エラー（state=" + String(s.mqttErrorCode) + "）";
        color = display.color565(248, 113, 113);
        break;
      default:
        state = "接続を試しています…";
        break;
    }
    if (y + 42 <= height - 22) {
      block("プリンター " + configPortal.config().bambuHost, state, color,
            String());
    }
  } else if (y + 42 <= height - 22) {
    block("プリンター", "未設定（Web の設定画面で登録）", sub, String());
  }

  display.setTextDatum(bottom_center);
  display.setTextColor(sub, bg);
  display.drawString("タップで閉じる", width / 2, height - 4);
  display.pushSprite(0, 0);
}

// SETTINGS情報画面を閉じる。APを停止してアバターを再開する。
void closeSettingsInfo() {
  configPortal.stopSettingsAp(); // APを停止してSTAモードに戻す
  settingsInfoOpen = false;
  avatarFace.resumeDrawing();
  avatarFace.resetToDefault();
}

// 本体の設定画面（Wi-Fi・プリンター）を開く。保存したら再起動する。
// メニューから来たときは顔の描画を止めたまま切り替える（再開直後に止めないため）。
using DeviceSetupScreen = bool (*)(M5Canvas&, ConfigPortal&,
                                   const std::function<void()>&);
void showSetupPortalStatus();

void openDeviceSetup(DeviceSetupScreen screen) {
  const bool fromMenu = modeMenuOpen;
  modeMenuOpen = false;
  modeMenuPressed = ModeMenuButton::None;
  deviceSetupOpen = true;
  if (!fromMenu) {
    avatarFace.pauseDrawing();
    delay(20);
  }
  const bool saved = screen(startupCanvas(), configPortal, serviceApp);
  if (saved) {
    Serial.println("[setup] saved on device, restarting");
    delay(300);
    ESP.restart();
  }
  deviceSetupOpen = false;
  displayWasTouching = false;
  avatarFace.resumeDrawing();
  avatarFace.resetToDefault();
  if (configPortal.isPortalActive()) showSetupPortalStatus();
}

// セットアップ AP 中の顔の表示。吹き出しは約18文字までなので、
// 「画面タップで設定」と「Web の設定画面」を3秒ごとに交互に出す。
uint32_t setupPortalStatusAt = 0;
bool setupPortalStatusAlt = false;

void showSetupPortalStatus() {
  setupPortalStatusAt = millis();
  setupPortalStatusAlt = false;
  avatarFace.showStatus("TAP: WIFI SETUP", 0);
}

// セットアップ AP 中（Wi-Fi 未接続）の画面タッチ: タップで Wi-Fi 設定を開く
void handleSetupPortalTouch() {
  const uint32_t now = millis();
  if (now - setupPortalStatusAt >= 3000) {
    setupPortalStatusAt = now;
    setupPortalStatusAlt = !setupPortalStatusAlt;
    avatarFace.showStatus(
        setupPortalStatusAlt ? "SETUP: 192.168.4.1" : "TAP: WIFI SETUP", 0);
  }
  int16_t x = 0;
  int16_t y = 0;
  const bool touching = M5.Display.getTouch(&x, &y);
  if (!touching && displayWasTouching) {
    displayWasTouching = false;
    openDeviceSetup(runWifiSetup);
    return;
  }
  displayWasTouching = touching;
}

// 顔の代わりにプリンター詳細画面を出す（メニュー・設定画面を開いていなければ）
void showPrinterScreenIfFree() {
  if (!bambu.isEnabled() || modeMenuOpen || settingsInfoOpen || deviceSetupOpen) return;
  openPrinterScreen();
}

// メニューから顔に戻さずにプリンター詳細画面へ切り替える
// （再開直後の一時停止で描画タスクをフレーム途中で止めないため）
void switchMenuToPrinterScreen() {
  modeMenuOpen = false;
  modeMenuPressed = ModeMenuButton::None;
  printerScreenOpen = true;
  printerScreenRenderer.invalidate();
  drawPrinterScreenNow();
}

// 設定（Wi-Fi・プリンター・接続情報）。メニューから顔の描画を止めたまま開く
void openSettingsMenu() {
  const bool fromMenu = modeMenuOpen;
  modeMenuOpen = false;
  modeMenuPressed = ModeMenuButton::None;
  deviceSetupOpen = true;
  if (!fromMenu) {
    avatarFace.pauseDrawing();
  }
  while (true) {
    const SettingsChoice choice = runSettingsMenu(startupCanvas(), configPortal, serviceApp);
    bool saved = false;
    if (choice == SettingsChoice::Wifi) {
      saved = runWifiSetup(startupCanvas(), configPortal, serviceApp);
    } else if (choice == SettingsChoice::Printer) {
      saved = runPrinterSetup(startupCanvas(), configPortal, serviceApp);
    } else if (choice == SettingsChoice::Info) {
      // 接続情報は今までどおり（タップで閉じる・設定用ホットスポットを立てる）
      deviceSetupOpen = false;
      settingsInfoOpen = true;
      configPortal.startSettingsAp();
      drawSettingsInfo();
      displayWasTouching = false;
      return;
    } else {
      break;  // 閉じる
    }
    if (saved) {
      Serial.println("[setup] saved on device, restarting");
      delay(300);
      ESP.restart();
    }
  }
  deviceSetupOpen = false;
  displayWasTouching = false;
  avatarFace.resumeDrawing();
  avatarFace.resetToDefault();
}

// メニューの見出しにお知らせを少し出す（メニューは開いたまま）
void showModeMenuHint(const char* text) {
  modeMenuHint = text;
  modeMenuHintUntil = millis() + 3000;
  drawModeMenu(ModeMenuButton::None);
}

// メニューを開く。アバターの描画を一時停止して画面を上書きする。
void openModeMenu() {
  if (modeMenuOpen) {
    return;
  }
  modeMenuOpen = true;
  modeMenuPressed = ModeMenuButton::None;
  avatarFace.pauseDrawing();
  delay(20);
  drawModeMenu(modeMenuPressed);
}

// メニューを閉じる。アバターの描画を再開してデフォルト状態に戻す。
void closeModeMenu() {
  if (!modeMenuOpen) {
    return;
  }
  modeMenuOpen = false;
  modeMenuPressed = ModeMenuButton::None;
  avatarFace.resumeDrawing();
  avatarFace.resetToDefault();
}

// タッチスクリーンの入力を処理する。
// 下からの上スワイプ（高さ44px以内から70px以上、横ブレ100px以内、1.2秒以内）でメニューを開く。
// 戻り値: メニューかSETTINGS画面が処理を消費した場合はtrue（他の入力処理をスキップする）
bool handleDisplayTouch() {
  int16_t x = 0;
  int16_t y = 0;
  const bool touching = M5.Display.getTouch(&x, &y);

  // FFT owns the LCD until the header is tapped or the bottom is swiped up.
  // Return false so MQTT/API speech and the service loop continue normally.
  if (usb_spectrum::isOpen()) {
    if (touching && !displayWasTouching) {
      displayTouchStartX = displayTouchLastX = x;
      displayTouchStartY = displayTouchLastY = y;
      displayTouchStartedAt = millis();
    } else if (touching) {
      displayTouchLastX = x; displayTouchLastY = y;
    } else if (displayWasTouching) {
      const int dx = abs(displayTouchLastX - displayTouchStartX);
      const int dy = displayTouchStartY - displayTouchLastY;
      const uint32_t duration = millis() - displayTouchStartedAt;
      if ((displayTouchStartY < 34 && abs(dy) < 20 && dx < 20) ||
          (displayTouchStartY >= 196 && dy >= 70 && dx <= 100 && duration <= 1200)) {
        usb_spectrum::close();
        avatarFace.resumeDrawing();
        avatarFace.resetToDefault();
      } else if (abs(dy) < 20 && dx < 20 && duration <= 600) {
        usb_spectrum::tap(displayTouchStartX, displayTouchStartY);
      }
    }
    displayWasTouching = touching;
    return false;
  }

  // プリンタ詳細画面: タップで顔に戻る（実況や頭タッチは止めないので false を返す）
  if (printerScreenOpen) {
    if (!touching && displayWasTouching) {
      closePrinterScreen();
    }
    displayWasTouching = touching;
    return false;
  }

  // SETTINGS情報画面が開いている: タッチリリースで閉じる
  if (settingsInfoOpen) {
    if (!touching && displayWasTouching) {
      closeSettingsInfo();
    }
    displayWasTouching = touching;
    return true; // 他の入力処理をブロック
  }

  // モードメニューが開いている: ボタンのハイライトと選択処理
  if (modeMenuOpen) {
    if (modeMenuHintUntil != 0 &&
        static_cast<int32_t>(millis() - modeMenuHintUntil) >= 0) {
      modeMenuHintUntil = 0;
      drawModeMenu(modeMenuPressed);
    }
    const ModeMenuButton current =
        touching ? modeMenuButtonAt(x, y) : ModeMenuButton::None;
    // ボタンが変わったら再描画してハイライト更新
    if (touching && current != modeMenuPressed) {
      modeMenuPressed = current;
      drawModeMenu(modeMenuPressed);
    }
    if (!touching && displayWasTouching) {
      const ModeMenuButton selected = modeMenuPressed;
      modeMenuPressed = ModeMenuButton::None;
      // プリンターが未設定のときは、設定の場所を案内するだけ（同じ設定画面を何か所からも開かない）
      if (!bambu.isEnabled() && (selected == ModeMenuButton::Printer ||
                                 selected == ModeMenuButton::PrinterMode)) {
        showModeMenuHint("設定 → プリンターで登録してね");
        displayWasTouching = touching;
        return true;
      }
      if (selected == ModeMenuButton::Fft) {
        // Keep face drawing suspended across the menu -> FFT transition.
        if (!usb_spectrum::open()) {
          showModeMenuHint("FFTを初期化できません");
          displayWasTouching = touching;
          return true;
        }
        modeMenuOpen = false;
        modeMenuPressed = ModeMenuButton::None;
        petReaction.cancel();
        petFeedbackPending = petRestorePrinterScreen = false;
        displayWasTouching = touching;
        return true;
      }
      if (selected == ModeMenuButton::Settings) {
        openSettingsMenu();
        return true;
      }
      if (selected == ModeMenuButton::Printer) {
        switchMenuToPrinterScreen();
        displayWasTouching = touching;
        return true;
      }
      if (selected == ModeMenuButton::PrinterMode) {
        // MQTT モードはプリンターの画面で始める（顔を再開せずに切り替える）
        selectModeManually(AppMode::Printer);
        switchMenuToPrinterScreen();
        displayWasTouching = touching;
        return true;
      }
      if (selected == ModeMenuButton::None) {
        displayWasTouching = touching;
        return true;
      }
      closeModeMenu(); // メニューを閉じてからモードを切り替える
      if (selected == ModeMenuButton::Voice) {
        const bool voice = !configPortal.config().commentaryVoice;
        configPortal.setCommentaryVoice(voice);
        avatarFace.showStatus(voice ? "VOICE ON" : "VOICE OFF", 1800);
        showStatusLed(voice ? 0 : 48, voice ? 64 : 24, voice ? 24 : 0);
      } else if (selected == ModeMenuButton::LocalLlm) {
        selectModeManually(AppMode::LocalLlm);
      } else if (selected == ModeMenuButton::LevelHold) {
        selectModeManually(AppMode::LevelHold);
      }
    }
    displayWasTouching = touching;
    return true; // 他の入力処理をブロック
  }

  // メニューもSETTINGSも開いていない: スワイプジェスチャーを検出する
  if (touching && !displayWasTouching) {
    // タッチ開始座標と時刻を記録
    displayTouchStartX = x;
    displayTouchStartY = y;
    displayTouchLastX = x;
    displayTouchLastY = y;
    displayTouchStartedAt = millis();
  } else if (touching) {
    // タッチ中は現在座標を更新し続ける
    displayTouchLastX = x;
    displayTouchLastY = y;
  } else if (!touching && displayWasTouching) {
    const int16_t height = M5.Display.height();
    const int16_t verticalTravel =
        displayTouchStartY - displayTouchLastY;    // 上方向が正（開始Y - 終了Y）
    const int16_t horizontalTravel =
        abs(displayTouchLastX - displayTouchStartX); // 横ブレの絶対値
    const uint32_t duration = millis() - displayTouchStartedAt;

    // 上スワイプ判定:
    //   - 画面下部44px以内から開始
    //   - 70px以上上に移動
    //   - 横ブレ100px以下
    //   - 所要時間1.2秒以下
    if (displayTouchStartY >= height - 44 &&
        verticalTravel >= 70 &&
        horizontalTravel <= 100 &&
        duration <= 1200) {
      openModeMenu();
      displayWasTouching = false;
      return true;
    }

    // 顔を軽くタップ: プリンタ詳細画面を開く（プリンタ監視中のみ）
    if (bambu.isEnabled() && abs(verticalTravel) < 20 &&
        horizontalTravel < 20 && duration <= 600 &&
        displayTouchStartY < height - 44) {
      petReaction.cancel();
      petFeedbackPending = petRestorePrinterScreen = false;
      openPrinterScreen();
      displayWasTouching = false;
      return false;
    }
  }

  displayWasTouching = touching;
  return false;
}

// 起動直後に表示するフォールバック顔（アバター起動前の暫定表示）。
// StackChan BSPの初期化中もこの顔が表示され続ける。
void drawBootFallbackFace() {
  auto cfg = M5.config();
  M5.begin(cfg);

  if (M5.Display.width() < M5.Display.height()) {
    M5.Display.setRotation(1); // 横向きに設定
  }
  M5.Display.setBrightness(160);
  M5.Display.fillScreen(TFT_BLACK);

  const int cx = M5.Display.width() / 2;
  const int cy = M5.Display.height() / 2;
  // シンプルな2つの白い円（目）と白い丸角矩形（口）で顔を描く
  M5.Display.fillCircle(cx - 64, cy - 18, 12, TFT_WHITE); // 左目
  M5.Display.fillCircle(cx + 64, cy - 18, 12, TFT_WHITE); // 右目
  M5.Display.fillRoundRect(cx - 42, cy + 30, 84, 7, 3, TFT_WHITE); // 口
}

// タッチ座標から起動時サーボ選択ダイアログのボタンを判定する。
// 左半分=NO、右半分=YES。ボタン領域: y=160〜226。
ServoPromptButton servoPromptButtonAt(int16_t x, int16_t y) {
  constexpr int16_t kButtonTop = 160;
  constexpr int16_t kButtonBottom = 226;

  if (y < kButtonTop || y > kButtonBottom) {
    return ServoPromptButton::None;
  }
  return x < M5.Display.width() / 2 ? ServoPromptButton::No
                                    : ServoPromptButton::Yes;
}

// 起動時サーボ選択ダイアログを描画する。
// 赤いNOボタン（左）と緑のYESボタン（右）を表示する。
void drawServoStartupPrompt(ServoPromptButton pressed, int secondsLeft = -1) {
  auto& display = startupCanvas();
  const int16_t width = display.width();
  constexpr int16_t kMargin = 14;
  constexpr int16_t kGap = 10;
  constexpr int16_t kButtonTop = 160;
  constexpr int16_t kButtonHeight = 66;
  const int16_t buttonWidth =
      (width - kMargin * 2 - kGap) / 2;
  const int16_t noX = kMargin;
  const int16_t yesX = noX + buttonWidth + kGap;

  // 押下中は明るい色、通常は暗い色
  const uint16_t noColor =
      pressed == ServoPromptButton::No ? 0x7800 : 0x4208;  // 赤 / 暗いグレー
  const uint16_t yesColor =
      pressed == ServoPromptButton::Yes ? 0x03E0 : 0x0260; // 緑 / 暗い緑

  display.fillScreen(TFT_BLACK);
  display.setFont(&fonts::Font2);
  display.setTextDatum(middle_center);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.setTextSize(2);
  display.drawString("SERVO STARTUP", width / 2, 30);

  display.setTextSize(1);
  display.drawString("Run full calibration?", width / 2, 76);
  display.setTextColor(TFT_YELLOW, TFT_BLACK); // 警告は黄色で強調
  display.drawString("Clear the area before selecting YES", width / 2, 111);
  display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  if (secondsLeft >= 0) {
    display.drawString("Auto NO in " + String(secondsLeft) + "s", width / 2, 137);
  } else {
    display.drawString("Touch a button to continue", width / 2, 137);
  }

  display.fillRoundRect(
      noX, kButtonTop, buttonWidth, kButtonHeight, 10, noColor);
  display.drawRoundRect(
      noX, kButtonTop, buttonWidth, kButtonHeight, 10, TFT_WHITE);
  display.fillRoundRect(
      yesX, kButtonTop, buttonWidth, kButtonHeight, 10, yesColor);
  display.drawRoundRect(
      yesX, kButtonTop, buttonWidth, kButtonHeight, 10, TFT_WHITE);

  display.setTextColor(TFT_WHITE);
  display.setTextSize(2);
  display.drawString("NO", noX + buttonWidth / 2, kButtonTop + 33);
  display.drawString("YES", yesX + buttonWidth / 2, kButtonTop + 33);
  display.pushSprite(0, 0);
}

// 起動時サーボ選択ダイアログを表示してユーザの選択を待つ。
// ブロッキング処理（アバター描画を一時停止してダイアログを表示）。
// 戻り値: NO=KeepPosition、YES=Calibrate
ServoStartupChoice askServoStartupChoice() {
  avatarFace.pauseDrawing();
  delay(40);
  // 停電復帰などの無人起動で止まらないよう、触れられなければ一定時間後に NO で進む
  constexpr uint32_t kPromptTimeoutMs = 10000;
  int secondsLeft = static_cast<int>(kPromptTimeoutMs / 1000);
  drawServoStartupPrompt(ServoPromptButton::None, secondsLeft);

  int16_t touchX = 0;
  int16_t touchY = 0;

  // 起動中に画面に触れていた指をリリースするまで待つ（誤操作防止）
  while (M5.Display.getTouch(&touchX, &touchY)) {
    M5StackChan.update();
    delay(10);
  }

  bool wasTouching = false;
  bool everTouched = false;
  ServoPromptButton pressed = ServoPromptButton::None;
  const uint32_t promptStartedAt = millis();

  while (true) {
    M5StackChan.update();
    if (!everTouched) {
      const uint32_t elapsed = millis() - promptStartedAt;
      if (elapsed >= kPromptTimeoutMs) {
        avatarFace.resumeDrawing();
        avatarFace.resetToDefault();
        return ServoStartupChoice::KeepPosition;
      }
      const int left =
          static_cast<int>((kPromptTimeoutMs - elapsed + 999) / 1000);
      if (left != secondsLeft) {
        secondsLeft = left;
        drawServoStartupPrompt(pressed, secondsLeft);
      }
    }
    const bool touching = M5.Display.getTouch(&touchX, &touchY);
    const ServoPromptButton current =
        touching ? servoPromptButtonAt(touchX, touchY)
                 : ServoPromptButton::None;
    everTouched |= touching;

    // ボタンが変わったら再描画してハイライト更新
    if (touching && current != pressed) {
      pressed = current;
      drawServoStartupPrompt(pressed);
    }

    // 指を離したとき、押していたボタンで判定する
    if (!touching && wasTouching) {
      const ServoPromptButton selected = pressed;
      pressed = ServoPromptButton::None;
      drawServoStartupPrompt(pressed);

      if (selected == ServoPromptButton::No) {
        avatarFace.resumeDrawing();
        avatarFace.resetToDefault();
        return ServoStartupChoice::KeepPosition;
      }
      if (selected == ServoPromptButton::Yes) {
        avatarFace.resumeDrawing();
        avatarFace.resetToDefault();
        return ServoStartupChoice::Calibrate;
      }
    }

    wasTouching = touching;
    delay(10);
  }
}

// セットアップ処理。電源投入後に1回だけ実行される。
void setup() {
  Serial.begin(115200);
  delay(100);
  const esp_reset_reason_t resetReason = esp_reset_reason();
  bootResetReason = resetReasonLabel(resetReason); // /status で表示する
  Serial.printf("\nStackChan boot %s, reset reason=%d (%s)\n", kAppVersion,
                static_cast<int>(resetReason), bootResetReason.c_str());

  // まず最初にディスプレイを起動してフォールバック顔を表示する。
  // StackChan BSPの初期化（次のステップ）はボディ配線の問題で時間がかかることがあるため、
  // 画面を先に出しておくことでハング時もユーザに状態が分かるようにする。
  drawBootFallbackFace();
  Serial.println("Fallback face ready");

  Serial.println("Initializing StackChan BSP...");
  M5StackChan.begin(); // サーボ・LED・タッチセンサ・IMU等の初期化
  Serial.println("StackChan BSP ready");

  // サーボAPIは有効にしつつ、不意な動きを防ぐためトルクと電源はOFFにする
  M5StackChan.Motion.setTorqueEnabled(false);
  M5StackChan.setServoPowerEnabled(false);
  M5StackChan.showRgbColor(0, 0, 0); // 全LED消灯
  Serial.println("Servo torque and power disabled");

  // NVSからキャリブレーションデータを読み込む
  calibrationController.load();
  const auto& savedCalibration = calibrationController.data();
  Serial.printf(
      "Calibration servo=%d imu=%d yaw=[%d,%d] pitch=[%d,%d] "
      "level=(%.3f,%.3f)\n",
      savedCalibration.servoValid, savedCalibration.imuLevelValid,
      savedCalibration.yawMin, savedCalibration.yawMax,
      savedCalibration.pitchMin, savedCalibration.pitchMax,
      savedCalibration.levelRollDeg,
      savedCalibration.levelPitchDeg);

  if (M5.Display.width() < M5.Display.height()) {
    M5.Display.setRotation(1); // 横向きに設定
  }
  M5.Display.setBrightness(160);
  M5.Display.fillScreen(TFT_BLACK);

  // WiFi接続前にアバターを起動しておく。
  // WiFi接続（最大15秒）や設定APモード中も顔が表示され続ける。
  Serial.println("Starting avatar...");
  avatarFace.begin();
  avatarFace.resetToDefault();
  avatarFace.showStatus("BOOT", 1200);
  Serial.println("Avatar started");

  // 起動時サーボ選択ダイアログ（キャリブレーションするかどうか）
  const ServoStartupChoice servoChoice = askServoStartupChoice();
  if (servoChoice == ServoStartupChoice::Calibrate) {
    Serial.println("Servo startup choice: CALIBRATE");
    bodyMotionSkipAutoStart = true;
    const bool calibrationOk =
        calibrationController.run(avatarFace); // フルキャリブレーション実行
    bodyMotionSkipAutoStart = !calibrationOk;
  } else {
    Serial.println("Servo startup choice: NO");
    bodyMotionSkipAutoStart = false;
    M5StackChan.Motion.setTorqueEnabled(false);
    M5StackChan.setServoPowerEnabled(false);
    avatarFace.showStatus("SERVO: NO", 1800);
  }

  M5.Speaker.begin();
  M5.Speaker.setVolume(255); // スピーカー音量（0〜255、最大）
  if (!usb_spectrum::begin()) {
    Serial.println("[usb] audio/spectrum initialization failed");
  } else {
    Serial.println("[usb] StackChan USB Spectrum: UAC2 48kHz/16bit/stereo ready");
  }
  // TTS再生中のコールバックを登録する
  ttsClient.setCallbacks(setLipSyncLevel, serviceApp);
  // 内蔵ボイス（LittleFS のボイスパック）。無くても動作は続ける
  builtinVoice.setCallbacks(setLipSyncLevel, serviceApp);
  if (!builtinVoice.begin()) {
    Serial.printf("[voice] builtin voice unavailable: %s\n",
                  builtinVoice.lastError().c_str());
  }

  // Gateway 連携用のコールバックを登録する（configPortal.begin() の前に設定）。
  // /api/speak: 話し中・発話待ちなら busy。受け付けたら発話待ちフラグを立てる。
  configPortal.setSpeakRequestHandler([](const String& text) -> bool {
    if (speaking || pendingApiSpeak) {
      return false; // busy
    }
    pendingApiSpeakText = text; // 空なら loop() 側で "__CURRENT__" を使う
    pendingApiSpeak = true;
    return true;
  });
  // /api/status: 現在の発話状態を返すためのプローブ
  configPortal.setSpeakingProbe([]() -> bool {
    return speaking || pendingApiSpeak;
  });

  // プリンタ ダッシュボード（Web）用 API。
  // ハンドラは TTS 再生中にも呼ばれるため、喋る処理は実況の待ち行列に積むだけにする。
  PrinterWebApi printerApi;
  printerApi.stateJson = []() {
    return printerStateJson(bambu.snapshot(), commentator, bambu.isEnabled(),
                            configPortal.config().commentaryVoice,
                            appModeKey(currentMode),
                            configPortal.config().autoPrinterMode);
  };
  printerApi.report = []() {
    Comment c = commentator.statusReport(bambu.snapshot());
    c.priority = CommentPriority::High;
    commentator.enqueue(c);
  };
  printerApi.refresh = []() { bambu.requestPushAll(); };
  printerApi.light = [](bool on) { bambu.requestChamberLight(on); };
  printerApi.say = [](const String& text) -> bool {
    Comment c;
    c.text = text;
    c.mood = CommentMood::Happy;
    c.priority = CommentPriority::High;
    commentator.enqueue(c);
    return true;
  };
  printerApi.voice = [](bool on) { configPortal.setCommentaryVoice(on); };
  // モード切り替えはサーボを動かすので、ここでは受け付けるだけにして loop() で行う
  printerApi.mode = [](const String& mode) {
    pendingModeRequest = static_cast<int8_t>(
        mode == "mqtt"    ? AppMode::Printer
        : mode == "level" ? AppMode::LevelHold
                          : AppMode::LocalLlm);
  };
  configPortal.setPrinterApi(printerApi);

  Serial.println("Starting network configuration...");
  const bool wifiConnected = configPortal.begin();
  M5.Speaker.setVolume(configPortal.config().speakerVolume);  // 保存済みの音量

  // ゲーミングRGB（顔の虹色循環）を設定値に従って有効化する
  avatarFace.setGamingRgb(configPortal.config().gamingRgb);

  // --- プリンタ監視と実況 ---
  const AppConfig& appConfig = configPortal.config();
  CommentarySettings commentary;
  commentary.progressStep = appConfig.commentaryStep;
  commentary.periodicMin = appConfig.commentaryPeriodMin;
  commentary.stages = appConfig.commentaryStages;
  commentary.temps = appConfig.commentaryTemps;
  commentator.configure(commentary);

  BambuConfig bambuConfig;
  bambuConfig.enabled = appConfig.bambuEnabled && wifiConnected;
  bambuConfig.host = appConfig.bambuHost;
  bambuConfig.serial = appConfig.bambuSerial;
  bambuConfig.accessCode = appConfig.bambuAccessCode;
  bambu.begin(bambuConfig);  // 専用タスクで接続・再接続し続ける
  updatePrinterHud(bambu.snapshot());

  if (wifiConnected) {
    // 完成予定時刻の表示用に時刻を合わせる（バックグラウンドで同期）
    configTzTime(appConfig.timezone.c_str(), "ntp.nict.jp", "time.google.com",
                 "pool.ntp.org");
  }

  if (!wifiConnected) {
    // WiFi接続失敗: セットアップAPモードで起動
    Serial.printf("Setup AP: %s, http://192.168.4.1\n",
                  configPortal.accessPointName().c_str());
    avatarFace.resetToDefault();
    showSetupPortalStatus(); // 常時表示
    showStatusLed(48, 32, 0); // 橙色LED: セットアップ待ち
    greetOnBoot();
    // はじめての起動（Wi-Fi 未設定）は、そのまま本体の Wi-Fi 設定画面を開く
    if (configPortal.config().wifiSsid.isEmpty()) {
      openDeviceSetup(runWifiSetup);
    }
    return; // ループに入る（APモードでは画面タップで Wi-Fi 設定を開ける）
  }

  // WiFi接続成功
  Serial.printf("StackChan IP: %s\n",
                configPortal.localIp().toString().c_str());
  Serial.printf("TTS: %s http://%s:%u speaker=%s\n",
                configPortal.config().ttsEngineType.c_str(),
                configPortal.config().ttsHost.c_str(),
                configPortal.config().ttsPort,
                configPortal.config().ttsSpeaker.c_str());

  avatarFace.resetToDefault();
  avatarFace.showStatus("LOCAL LLM");
  showStatusLed(0, 48, 0); // 緑色LED: 正常動作中
  startBodyMotion();
  // 印刷中なら、状態が届いたところで updateAutoPrinterMode() が MQTT モードへ移す
  Serial.println("Mode default: LOCAL LLM");
  greetOnBoot();
  petInteractionReady = true;
}

// メインループ。約5ms間隔で繰り返し実行される。
void loop() {
  serviceApp(); // 全サブシステムの更新

  // APセットアップモード中は、画面タップで本体の Wi-Fi 設定を開く以外の操作はしない
  // （Web の設定画面 http://192.168.4.1 も使える）
  if (configPortal.isPortalActive()) {
    handleSetupPortalTouch();
    delay(5);
    return;
  }

  // タッチスクリーンの処理（メニュー・スワイプ）が入力を消費した場合はスキップ
  if (handleDisplayTouch()) {
    delay(5);
    return;
  }

  // 本体のプリンタ詳細画面を開いていれば描き直す
  refreshPrinterScreen();

  // Web からのモード切り替え・印刷開始/終了による MQTT モードの自動切り替え
  if (pendingModeRequest >= 0 && !speaking) {
    const AppMode requested = static_cast<AppMode>(pendingModeRequest);
    pendingModeRequest = -1;
    if (requested != AppMode::Printer || bambu.isEnabled()) {
      selectModeManually(requested);
      if (requested == AppMode::Printer) showPrinterScreenIfFree();
    }
  }
  updateAutoPrinterMode(millis());

  // Gateway からの /api/speak 発話待ちを処理する（ブラウザ送信→自動発話）。
  // WebServer ハンドラ内ではフラグを立てるだけで、実際の再生はここで行う。
  if (pendingApiSpeak && !speaking) {
    pendingApiSpeak = false;
    String text = pendingApiSpeakText;
    pendingApiSpeakText = "";
    if (text.isEmpty()) {
      text = "__CURRENT__"; // 既定: Gateway の current.wav を再生する
    }
    speakText(text);
    delay(5);
    return;
  }

  // プリンタ実況: 待ち行列のコメントを順番に演出付きで喋る。
  // High（完了・エラー等）は字幕だけの表示待ちを飛ばしてすぐ出す。
  // MQTT モード以外では細かい実況（Low: 工程・温度・定期報告など）は喋らず、
  // Web の実況ログにだけ残す。
  if (!speaking && commentator.hasComment() &&
      (!petReaction.active(millis()) ||
       commentator.peekPriority() == CommentPriority::High) &&
      (static_cast<int32_t>(millis() - commentBusyUntil) >= 0 ||
       commentator.peekPriority() == CommentPriority::High)) {
    Comment comment = commentator.popComment();
    if (currentMode != AppMode::Printer &&
        comment.priority == CommentPriority::Low) {
      comment.createdAt = millis();
      commentator.remember(comment);
    } else {
      performComment(comment);
    }
    delay(5);
    return;
  }

  if (performPetResponse()) {
    delay(5);
    return;
  }

  // ボタンA: 設定テキストをTTSで読み上げる（"__REASK_LAST__" 運用で最後の質問を再LLM）
  if (!usb_spectrum::isOpen() && M5.BtnA.wasPressed()) {
    speakConfiguredText();
  }

  // ボタンB: 次の表情に切り替える（TTS中は無効）
  if (!speaking && !usb_spectrum::isOpen() && M5.BtnB.wasPressed()) {
    avatarFace.nextExpression();
  }

  // ボタンC: 次の顔型に切り替える（TTS中は無効）
  if (!speaking && !usb_spectrum::isOpen() && M5.BtnC.wasPressed()) {
    avatarFace.nextFace();
  }

  // 頭部タッチセンサの処理（TTS中は無効）
  if (!speaking && !usb_spectrum::isOpen()) {
    handleTopTouch();
  }

  delay(5);
}
