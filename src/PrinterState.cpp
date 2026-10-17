#include "PrinterState.h"

#include <math.h>

PrintPhase printPhaseFromString(const char* value) {
  if (value == nullptr || value[0] == '\0') return PrintPhase::Unknown;
  if (strcmp(value, "IDLE") == 0) return PrintPhase::Idle;
  if (strcmp(value, "PREPARE") == 0) return PrintPhase::Prepare;
  if (strcmp(value, "SLICING") == 0) return PrintPhase::Slicing;
  if (strcmp(value, "RUNNING") == 0) return PrintPhase::Running;
  if (strcmp(value, "PAUSE") == 0) return PrintPhase::Pause;
  if (strcmp(value, "FINISH") == 0) return PrintPhase::Finish;
  if (strcmp(value, "FAILED") == 0) return PrintPhase::Failed;
  return PrintPhase::Unknown;
}

const char* printPhaseLabelJa(PrintPhase phase) {
  switch (phase) {
    case PrintPhase::Idle:    return "待機中";
    case PrintPhase::Prepare: return "準備中";
    case PrintPhase::Slicing: return "スライス中";
    case PrintPhase::Running: return "印刷中";
    case PrintPhase::Pause:   return "一時停止";
    case PrintPhase::Finish:  return "完了";
    case PrintPhase::Failed:  return "失敗";
    default:                  return "不明";
  }
}

const char* printPhaseLabelEn(PrintPhase phase) {
  switch (phase) {
    case PrintPhase::Idle:    return "IDLE";
    case PrintPhase::Prepare: return "PREPARE";
    case PrintPhase::Slicing: return "SLICING";
    case PrintPhase::Running: return "RUNNING";
    case PrintPhase::Pause:   return "PAUSE";
    case PrintPhase::Finish:  return "FINISH";
    case PrintPhase::Failed:  return "FAILED";
    default:                  return "UNKNOWN";
  }
}

// stg_cur の ID と意味は OpenBambuAPI / ha-bambulab のコミュニティ調査に基づく。
const char* stageLabelJa(int stage) {
  switch (stage) {
    case 0:  return "印刷中";
    case 1:  return "オートベッドレベリング中";
    case 2:  return "ベッドを予熱中";
    case 3:  return "振動補正中";
    case 4:  return "フィラメント交換中";
    case 5:  return "M400で一時停止中";
    case 6:  return "フィラメント切れで一時停止";
    case 7:  return "ノズルを加熱中";
    case 8:  return "押出キャリブレーション中";
    case 9:  return "ベッド表面をスキャン中";
    case 10: return "1層目を検査中";
    case 11: return "ビルドプレートを確認中";
    case 12: return "ライダー校正中";
    case 13: return "ヘッドを原点に移動中";
    case 14: return "ノズル先端を清掃中";
    case 15: return "押出機の温度を確認中";
    case 16: return "ユーザーが一時停止";
    case 17: return "フロントカバー外れで一時停止";
    case 18: return "ライダー校正中";
    case 19: return "流量キャリブレーション中";
    case 20: return "ノズル温度異常で一時停止";
    case 21: return "ベッド温度異常で一時停止";
    case 22: return "フィラメントを引き抜き中";
    case 23: return "脱調で一時停止";
    case 24: return "フィラメントを装填中";
    case 25: return "モーターノイズ校正中";
    case 26: return "AMS通信断で一時停止";
    case 27: return "ヒートブレイクファン異常で一時停止";
    case 28: return "チャンバー温度異常で一時停止";
    case 29: return "チャンバー冷却中";
    case 30: return "Gコードで一時停止";
    case 31: return "モーターノイズ披露中";
    case 32: return "ノズル巻き付き検出で一時停止";
    case 33: return "カッター異常で一時停止";
    case 34: return "1層目異常で一時停止";
    case 35: return "ノズル詰まりで一時停止";
    default: return nullptr;
  }
}

const char* speedLabelJa(SpeedLevel level) {
  switch (level) {
    case SpeedLevel::Silent:    return "サイレント";
    case SpeedLevel::Standard:  return "スタンダード";
    case SpeedLevel::Sport:     return "スポーツ";
    case SpeedLevel::Ludicrous: return "ルーディクラス";
    default:                    return "-";
  }
}

String hmsCodeString(const HmsEntry& entry) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%04X_%04X_%04X_%04X",
           (unsigned)(entry.attr >> 16), (unsigned)(entry.attr & 0xFFFF),
           (unsigned)(entry.code >> 16), (unsigned)(entry.code & 0xFFFF));
  return String(buf);
}

String remainingJa(int minutes) {
  if (minutes < 0) return String("不明");
  if (minutes < 1) return String("1分未満");
  if (minutes < 60) return String(minutes) + "分";
  const int h = minutes / 60;
  const int m = minutes % 60;
  if (m == 0) return String(h) + "時間";
  return String(h) + "時間" + String(m) + "分";
}

String remainingShort(int minutes) {
  if (minutes < 0) return String("--:--");
  char buf[12];
  snprintf(buf, sizeof(buf), "%d:%02d", minutes / 60, minutes % 60);
  return String(buf);
}

const char* colorNameJa(uint32_t rgb) {
  const float r = ((rgb >> 16) & 0xFF) / 255.0f;
  const float g = ((rgb >> 8) & 0xFF) / 255.0f;
  const float b = (rgb & 0xFF) / 255.0f;
  const float maxC = fmaxf(r, fmaxf(g, b));
  const float minC = fminf(r, fminf(g, b));
  const float delta = maxC - minC;
  const float v = maxC;
  const float s = maxC <= 0.0f ? 0.0f : delta / maxC;

  if (v < 0.18f) return "黒";
  if (s < 0.15f) {
    if (v > 0.85f) return "白";
    return "グレー";
  }

  float h = 0.0f;
  if (delta > 0.0f) {
    if (maxC == r) {
      h = 60.0f * fmodf((g - b) / delta, 6.0f);
    } else if (maxC == g) {
      h = 60.0f * ((b - r) / delta + 2.0f);
    } else {
      h = 60.0f * ((r - g) / delta + 4.0f);
    }
  }
  if (h < 0.0f) h += 360.0f;

  if (h < 15.0f || h >= 345.0f) return v < 0.5f ? "えんじ色" : "赤";
  if (h < 42.0f) return v < 0.6f ? "茶色" : "オレンジ";
  if (h < 70.0f) return v < 0.55f ? "オリーブ色" : "黄色";
  if (h < 165.0f) return "緑";
  if (h < 200.0f) return "水色";
  if (h < 255.0f) return "青";
  if (h < 290.0f) return "紫";
  return "ピンク";
}

bool isCancelError(uint32_t printError) {
  // 0x0300400C: "The task was canceled."（ユーザー操作による中止）
  return printError == 0x0300400CUL;
}
