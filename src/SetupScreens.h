#pragma once

#include <M5Unified.h>

#include <functional>

#include "ConfigPortal.h"

// 本体の画面だけで Wi-Fi とプリンター（MQTT）を設定する画面（タッチキーボードつき）。
// どちらもブロッキングで、待つ間も service() を呼び続ける。
// 戻り値: 設定を保存した=true（呼び出し側で再起動する）、やめた=false。

// Wi-Fi: 周辺のネットワークを一覧 → パスワード入力 → 実際につないで確かめてから保存
bool runWifiSetup(M5Canvas& canvas, ConfigPortal& portal,
                  const std::function<void()>& service);

// プリンター: 監視 ON/OFF・IP アドレス・シリアル番号・アクセスコード・自動 MQTT モード
bool runPrinterSetup(M5Canvas& canvas, ConfigPortal& portal,
                     const std::function<void()>& service);

// 設定メニュー（Wi-Fi・プリンター・接続情報）。選んだ項目を返す
enum class SettingsChoice { Close, Wifi, Printer, Info };
SettingsChoice runSettingsMenu(M5Canvas& canvas, ConfigPortal& portal,
                               const std::function<void()>& service);

// 「保存しました。再起動します」などの1枚だけのお知らせ画面
void drawSetupMessage(M5Canvas& canvas, const char* title, const String& line,
                      uint16_t color);
