#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

#include <functional>

// アプリ全体で共有する設定値。NVSから読み込み・保存される。
struct AppConfig {
  String wifiSsid;                              // 接続先WiFiのSSID
  String wifiPassword;                          // WiFiパスワード
  String ttsHost = "192.168.1.2";              // TTSサーバのIPアドレス
  uint16_t ttsPort = 50021;                    // TTSサーバのポート番号
  String ttsSpeaker = "3";                     // 話者ID（VoiceVox: ずんだもんノーマル=3）
  String ttsEngineType = "voicevox_compatible"; // TTSエンジン種別（builtin = 内蔵ボイス）
  String speechText =
      "Hello! I am Zundamon. I can now talk using StackChan!"; // Aボタンで話すデフォルトテキスト

  // カメラを使って明るい方向に目線を向ける機能。
  // CoreS3 ではカメラと内部I2C（タッチ等）がバスを共有するため、
  // タッチが効かなくなる/不安定な場合はこのスイッチをOFFにする。
  bool cameraGaze = true;

  // ゲーミングRGB演出。ONにすると顔（画面）と本体LEDが虹色にゆっくり循環する。
  // OFFにすると通常の2トーン表示＋ステータス色LEDに戻る。
  bool gamingRgb = true;

  // --- Bambu Lab プリンタ監視（LAN MQTT）---
  bool bambuEnabled = false;
  String bambuHost;         // プリンタの IP アドレス
  String bambuSerial;       // シリアル番号
  String bambuAccessCode;   // LAN アクセスコード（画面には再表示しない）
  bool autoPrinterMode = true;  // 印刷が始まったら自動で MQTT モードへ切り替える

  // --- 実況 ---
  bool commentaryVoice = true;       // 実況を喋る（OFF = 字幕と表情だけ）
  uint8_t commentaryStep = 10;       // 進捗実況の間隔（%、0 = しない）
  uint16_t commentaryPeriodMin = 15; // 無言が続いたときの定期報告（分、0 = しない）
  bool commentaryStages = true;      // 準備工程（レベリング・加熱など）を実況
  bool commentaryTemps = true;       // 目標温度到達を実況
  bool quietEnabled = true;          // 夜間は重要な実況だけ喋る
  uint8_t quietFrom = 23;            // 夜間の開始（時）
  uint8_t quietTo = 7;               // 夜間の終了（時）
  uint8_t speakerVolume = 255;       // スピーカー音量（0〜255）

  // --- 表示 ---
  bool printerHud = true;            // 顔の上にプリンタ HUD を重ねる
  bool ledProgress = true;           // 印刷中は本体 LED で進捗を表示
  String timezone = "JST-9";         // POSIX TZ（完成予定時刻の表示用）
};

// Web ダッシュボードからプリンタ機能を呼ぶためのフック（main.cpp が登録）
struct PrinterWebApi {
  std::function<String()> stateJson;          // GET  /api/printer
  std::function<void()> report;               // POST /api/printer/report
  std::function<void()> refresh;              // POST /api/printer/refresh
  std::function<void(bool)> light;            // POST /api/printer/light
  std::function<bool(const String&)> say;     // POST /api/printer/say
  std::function<void(bool)> voice;            // POST /api/printer/voice
  std::function<void(const String&)> mode;    // POST /api/mode（mqtt / llm / level）
};

// /statusページに表示するアプリ実行時の状態。
// main.cppからsetRuntimeStatus()で定期的に更新する。
struct RuntimeStatus {
  String appMode = "---";       // 現在のアプリモード名（"LOCAL LLM" 等）
  bool servoCalibrated = false; // サーボキャリブレーション済みかどうか
  bool imuCalibrated = false;   // IMUキャリブレーション済みかどうか

  // --- 診断情報（クラッシュ調査用）---
  String resetReason = "";      // 直近の再起動理由（PANIC/BROWNOUT/WDT等）
  uint32_t freeHeap = 0;        // 現在の空きヒープ（バイト）
  uint32_t minFreeHeap = 0;     // 起動以降の最小空きヒープ（バイト）
  uint32_t maxAllocHeap = 0;    // 確保可能な最大連続ブロック（フラグメント指標）
  uint32_t freePsram = 0;       // 空きPSRAM（バイト）
  uint32_t uiFrames = 0;       // プリンター詳細画面を描いた回数
  uint32_t uiPixels = 0;       // LCDへ転送したプリンター画面のピクセル数
  bool cameraActive = false;    // カメラ目線が動作中かどうか
  uint16_t voiceClips = 0;      // 内蔵ボイスのクリップ数（0 = 未書き込み）
  bool voiceFreeText = false;   // 内蔵ボイスでどんな文章も読める（モーラの音と読み辞書あり）
};

// WiFi接続・設定用Webサーバを管理するクラス。
//
// 動作フロー:
//   begin() 呼び出し時、保存済みSSIDで接続を試みる。
//   - 接続成功 → STAモードのままWebサーバ起動（ポート80）
//   - 接続失敗 → APモード（StackChan-Setup-XXXXXX）でWebサーバ起動
//
// ページ:
//   GET /         プリンタ ダッシュボード（セットアップAP中は設定フォーム）
//   GET /settings 設定フォーム（現在値を事前入力、パスワード類は伏せる）
//   POST /save    設定を保存して再起動
//   GET /status   システム状態の確認ページ（5秒自動更新）
//   GET /app.css  共通スタイルシート
class ConfigPortal {
 public:
  // NVSから設定を読み込み、WiFi接続またはAPを起動する。
  // 戻り値: WiFi接続成功=true、APモード=false
  bool begin();

  // Webサーバのリクエストを処理する（メインループから毎フレーム呼ぶ）
  void update();

  // セットアップ用APが起動中かどうか
  bool isPortalActive() const;

  // WiFiに接続中かどうか
  bool isConnected() const;

  // 現在の設定値を返す
  const AppConfig& config() const;

  // デバイスのIPアドレスを返す（AP時は192.168.4.1、STA時はDHCPアドレス）
  IPAddress localIp() const;

  // セットアップ用APのSSID名を返す（例: "StackChan-Setup-AABBCC"）
  String accessPointName() const;

  // SETTINGSメニュー用APを追加起動する（既存WiFi接続を維持したまま）
  // WIFI_AP_STAモードでAPを起動し、192.168.4.1でWebUIにアクセス可能にする
  void startSettingsAp();

  // SETTINGSメニュー用APを停止し、STAモードに戻す
  void stopSettingsAp();

  // SETTINGSメニュー用APが起動中かどうか
  bool isSettingsApActive() const;

  // APのIPアドレスを返す（通常 192.168.4.1）
  IPAddress settingsApIp() const;

  // /statusページに表示するアプリ実行時の状態を更新する（serviceApp()から定期呼び出し）
  void setRuntimeStatus(const RuntimeStatus& status);

  // POST /api/speak のハンドラを登録する。
  // 引数: 発話テキスト（空なら呼び出し側で既定値を使う）。
  // 戻り値: 受け付けた=true、話し中などで拒否=false（busy）。
  void setSpeakRequestHandler(std::function<bool(const String&)> handler);

  // GET /api/status の speaking フィールド用に、現在の発話状態を返すプローブを登録する。
  void setSpeakingProbe(std::function<bool()> probe);

  // プリンタ ダッシュボード用 API のフックを登録する
  void setPrinterApi(const PrinterWebApi& api);

  // 実況の音声 ON/OFF を変更して NVS に保存する（再起動不要）
  void setCommentaryVoice(bool enabled);

  // --- 本体の設定画面（タッチキーボード）用 ---

  // 指定の Wi-Fi に実際につないでみる（最大 timeoutMs、待つ間 service を呼ぶ）。
  // つながらなければ元の Wi-Fi へつなぎ直す。戻り値: つながった=true
  bool testWifi(const String& ssid, const String& password, uint32_t timeoutMs,
                const std::function<void()>& service);

  // Wi-Fi の接続先を NVS に保存する（反映は再起動後）
  void saveWifi(const String& ssid, const String& password);

  // プリンター（MQTT）の設定を NVS に保存する（反映は再起動後）。
  // accessCode が空なら保存済みのアクセスコードを残す。
  void saveBambu(bool enabled, const String& host, const String& serial,
                 const String& accessCode, bool autoMode);

 private:
  Preferences preferences_;          // ESP32 NVS（不揮発ストレージ）アクセス
  WebServer server_{80};             // ポート80のHTTPサーバ
  AppConfig config_;                 // 現在の設定値
  RuntimeStatus runtimeStatus_;      // アプリ実行時の状態（ステータスページ用）
  bool portalActive_ = false;        // セットアップAPが起動中かどうか
  bool settingsApActive_ = false;    // SETTINGSメニュー用APが起動中かどうか
  String accessPointName_;           // APのSSID名（例: "StackChan-Setup-AABBCC"）

  // Gateway 連携用コールバック
  std::function<bool(const String&)> speakRequestFn_; // POST /api/speak 受付
  std::function<bool()> speakingProbe_;               // GET /api/status の speaking
  PrinterWebApi printerApi_;                          // /api/printer/*

  // NVSから設定を読み込む
  void load();

  // NVSに設定を保存する
  void save();

  // 保存済みSSID/パスワードでWiFi接続を試みる（タイムアウト15秒）
  bool connectWifi();

  // セットアップ用APを起動する（接続失敗時）
  void startPortal();

  // APのSSID名を初期化する（未設定の場合のみ、MACアドレス末尾6桁を使用）
  void ensureAccessPointName();

  // WebサーバのURLルートを登録する（GET /、POST /save、GET /status）
  void registerRoutes();

  // 設定WebページのHTMLを生成する（現在値を事前入力、パスワードは伏せる）
  String pageHtml(const String& message = "");

  // システム状態ページのHTMLを生成する（5秒自動更新）
  String statusHtml();

  // Local LLM Chat カード（Gateway /ask をブラウザから直接叩くフロントエンド）を生成する
  String chatHtml();

  // 共通の <head> とヘッダー（ナビゲーション）を生成する
  String pageHead(const char* title, const char* active, bool autoRefresh = false);

  // WiFiスキャン結果を<option>タグのリストとして返す
  String wifiOptionsHtml();

  // GET /api/status が返す JSON 文字列を生成する
  String apiStatusJson();
};
