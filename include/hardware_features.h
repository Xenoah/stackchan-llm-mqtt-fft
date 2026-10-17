#pragma once

// StackChanボディ周辺機器:
// サーボ×2、RGB LED×12、3ゾーンタッチセンサ（頭部）、INA226バッテリモニタ
#include <M5StackChan.h>

// CoreS3本体周辺機器:
// カメラ、照度/近接センサ、ディスプレイ、タッチスクリーン、スピーカ、
// マイク×2、IMU（加速度・ジャイロ）、RTC、電源管理、内部/外部I2C
#include <M5CoreS3.h>

// StackChanボディの追加デバイス用ライブラリ
#include <IRrecv.h>           // IR受信
#include <IRremoteESP8266.h>  // IRプロトコル定義
#include <IRsend.h>           // IR送信
#include <M5UnitUnifiedNFC.h> // NFCリーダー/ライター

// ESP32/CoreS3 組み込み機能
#include <BLEDevice.h>  // Bluetooth LE
#include <SD.h>         // microSDカード
#include <WiFi.h>       // WiFi通信
#include <esp_camera.h> // カメラ制御

namespace stackchan_hardware {

// IR送信ピン番号（GPIO 5）
constexpr uint8_t kIrSendPin = 5;
// IR受信ピン番号（GPIO 10）
constexpr uint8_t kIrReceivePin = 10;
// StackChanボディI2CのSCLピン（GPIO 11）
constexpr uint8_t kBodyI2cSclPin = 11;
// StackChanボディI2CのSDAピン（GPIO 12）
constexpr uint8_t kBodyI2cSdaPin = 12;
// StackChanボディのRGB LED個数
constexpr uint8_t kRgbLedCount = 12;

// M5StackChan.begin() でボディデバイスを初期化する。
// カメラとLTR553（照度/近接センサ）はハードウェアリソースを消費するため、
// 使用時のみ明示的に初期化する:
//   CoreS3.Camera.begin();
//   CoreS3.Ltr553.begin(...);
//
// NFCとIRも同様にバスを占有するため、機能が必要になったタイミングで
// アプリケーション側が初期化する。ライブラリはインポート済みですぐに使える。

}  // namespace stackchan_hardware
