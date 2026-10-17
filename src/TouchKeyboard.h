#pragma once

#include <M5Unified.h>

#include <functional>

// 本体の画面に出すタッチキーボード（Wi-Fi・プリンターの設定用）。
//
// 画面いっぱい（320x240）の 16bit canvas に描く。英字（シフト・記号ページつき）と、
// IP アドレス用の数字キーパッドの2種類。パスワードは伏せ字で、最後に打った1文字だけ
// 1秒見せる（右上のボタンで全部表示に切り替えられる）。
enum class KeyboardLayout : uint8_t {
  Text,     // 英数字・記号（QWERTY）
  Numeric,  // 数字と「.」だけ（IP アドレス）
};

struct KeyboardOptions {
  const char* title = "";   // 上部の見出し（日本語可）
  String hint;              // 見出しの右に小さく出す補足（SSID など）
  KeyboardLayout layout = KeyboardLayout::Text;
  bool secret = false;      // パスワード（伏せ字）
  bool startUpper = false;  // 大文字固定で始める（シリアル番号など）
  size_t maxLength = 64;
};

// キーボードを表示して入力を受け付ける（ブロッキング）。
// 「決定」で value を書き換えて true、「×」で value はそのまま false を返す。
// 入力中も service() を呼び続けるので、Web サーバーや MQTT の取り込みは止まらない。
bool runTouchKeyboard(M5Canvas& canvas, String& value,
                      const KeyboardOptions& options,
                      const std::function<void()>& service);
