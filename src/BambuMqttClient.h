#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>

#include <atomic>

#include "PrinterState.h"

// Bambu Lab プリンタへの LAN MQTT 接続設定
struct BambuConfig {
  bool enabled = false;
  String host;        // プリンタの IP アドレス
  String serial;      // シリアル番号（例: 01P00A123456789）
  String accessCode;  // LAN アクセスコード（8桁）
};

// Bambu Lab LAN MQTT クライアント（P1S / P1P / X1 / A1 系）。
//
// プロトコルは Xenoah/ESP32-bambu-MQTT の PrinterComm を踏襲:
//   - mqtts://<host>:8883、ユーザー名 "bblp"、パスワード = アクセスコード
//   - 購読: device/<serial>/report、送信: device/<serial>/request
//   - 接続直後に {"pushing":{"command":"pushall"}} で全状態を要求
//   - 自己署名証明書のため setInsecure()、送信は retain=false
//
// ESP32-bambu-MQTT との違い:
//   - 専用 FreeRTOS タスクで動かし、TLS 接続や受信で顔・Web・TTS を止めない
//   - 接続失敗で停止せず、指数バックオフで再接続し続ける
//   - P1 シリーズの差分通知をマージして完全な状態を保持する
//   - ArduinoJson のフィルタで必要なフィールドだけを解析しヒープを節約する
class BambuMqttClient {
 public:
  // 設定を受け取り、有効かつ設定済みなら MQTT タスクを起動する
  void begin(const BambuConfig& config);

  // 設定が揃っていて監視が有効か
  bool isEnabled() const;

  // 最新状態のコピーを返す（スレッドセーフ）
  PrinterState snapshot() const;

  // 状態が更新されるたびに増える番号（変化検出用）
  uint32_t revision() const;

  // 全状態の再送（pushall）を要求する（連打は内部で間引く）
  void requestPushAll();

  // チャンバーライトの ON/OFF を要求する
  void requestChamberLight(bool on);

  // MQTT タスクのスタックの残り（最も少なかったときのバイト数。監視用、未起動なら 0）
  uint32_t stackFree() const;

 private:
  static void taskEntry(void* arg);
  void taskLoop();
  bool connectOnce();
  void onMessage(char* topic, uint8_t* payload, unsigned int length);
  void applyPrint(JsonObjectConst print);
  void publishPushAll();
  void publishChamberLight(bool on);
  bool publishJson(JsonDocument& doc);
  void setLink(LinkState link, int errorCode = 0);
  void commit();

  BambuConfig config_;
  bool enabled_ = false;
  TaskHandle_t task_ = nullptr;

  WiFiClientSecure tls_;
  PubSubClient mqtt_{tls_};

  // 受信スレッドだけが触る作業用状態。commit() で shared_ へコピーする。
  PrinterState work_;
  PrinterState shared_;
  mutable SemaphoreHandle_t mutex_ = nullptr;
  std::atomic<uint32_t> revision_{0};

  std::atomic<bool> pushAllRequested_{false};
  std::atomic<int8_t> lightRequest_{-1};
  uint32_t lastPushAllAt_ = 0;
  uint32_t sequenceId_ = 0;

  JsonDocument filter_;
  char topicReport_[64] = {};
  char topicRequest_[64] = {};
  char clientId_[32] = {};
};
