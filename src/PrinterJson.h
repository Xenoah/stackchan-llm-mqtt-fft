#pragma once

#include <Arduino.h>

#include "PrintCommentator.h"
#include "PrinterState.h"

// GET /api/printer の JSON を作る（Web ダッシュボード用）
// mode は "mqtt" / "llm" / "level"、autoMode は印刷開始で MQTT モードへ自動で入るか
String printerStateJson(const PrinterState& state,
                        const PrintCommentator& commentator, bool enabled,
                        bool voice, const char* mode, bool autoMode);

// 完成予定時刻を "15:42" 形式で返す（NTP 未同期・不明なら空文字）
String etaClockShort(int remainingMin);
