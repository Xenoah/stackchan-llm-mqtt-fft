#include "PrinterJson.h"

#include <ArduinoJson.h>
#include <math.h>
#include <time.h>

namespace {

const char* linkName(LinkState link) {
  switch (link) {
    case LinkState::WaitingWifi: return "waiting_wifi";
    case LinkState::Connecting:  return "connecting";
    case LinkState::Online:      return "online";
    case LinkState::Error:       return "error";
    default:                     return "disabled";
  }
}

const char* moodName(CommentMood mood) {
  switch (mood) {
    case CommentMood::Happy:  return "happy";
    case CommentMood::Sad:    return "sad";
    case CommentMood::Doubt:  return "doubt";
    case CommentMood::Angry:  return "angry";
    case CommentMood::Sleepy: return "sleepy";
    default:                  return "neutral";
  }
}

// NaN は JSON に出せないので null にする
void setTemp(JsonObject obj, const char* key, float value) {
  if (isnan(value)) {
    obj[key] = nullptr;
  } else {
    obj[key] = roundf(value * 10.0f) / 10.0f;
  }
}

void setOptionalInt(JsonObject obj, const char* key, int value) {
  if (value < 0) {
    obj[key] = nullptr;
  } else {
    obj[key] = value;
  }
}

}  // namespace

String etaClockShort(int remainingMin) {
  if (remainingMin < 0) return String();
  const time_t now = time(nullptr);
  if (now < 1700000000) return String();
  const time_t eta = now + static_cast<time_t>(remainingMin) * 60;
  struct tm t;
  localtime_r(&eta, &t);
  char buf[8];
  snprintf(buf, sizeof(buf), "%d:%02d", t.tm_hour, t.tm_min);
  return String(buf);
}

String printerStateJson(const PrinterState& s,
                        const PrintCommentator& commentator, bool enabled,
                        bool voice, const char* mode, bool autoMode) {
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  root["enabled"] = enabled;
  root["voice"] = voice;
  root["mode"] = mode;
  root["auto_mode"] = autoMode;
  root["link"] = linkName(s.link);
  if (s.link == LinkState::Error) root["mqtt_error"] = s.mqttErrorCode;
  root["synced"] = s.synced;
  root["age"] = s.lastMessageAt == 0
                    ? -1
                    : static_cast<int>((millis() - s.lastMessageAt) / 1000);

  root["phase"] = printPhaseLabelEn(s.phase);
  root["phase_ja"] = printPhaseLabelJa(s.phase);
  root["job"] = s.jobName;
  setOptionalInt(root, "percent", s.percent);
  setOptionalInt(root, "remaining_min", s.remainingMin);
  const String eta = etaClockShort(s.remainingMin);
  if (!eta.isEmpty()) root["eta"] = eta;
  setOptionalInt(root, "layer", s.layer);
  setOptionalInt(root, "total_layers", s.totalLayers);
  root["stage"] = s.stage;
  const char* stage = stageLabelJa(s.stage);
  if (stage != nullptr && s.isActive()) root["stage_ja"] = stage;
  if (s.speed != SpeedLevel::Unknown) root["speed"] = speedLabelJa(s.speed);

  setTemp(root, "nozzle", s.nozzleTemp);
  setTemp(root, "nozzle_target", s.nozzleTarget);
  setTemp(root, "bed", s.bedTemp);
  setTemp(root, "bed_target", s.bedTarget);
  setTemp(root, "chamber", s.chamberTemp);

  JsonObject fans = root["fans"].to<JsonObject>();
  setOptionalInt(fans, "part", s.partFan);
  setOptionalInt(fans, "aux", s.auxFan);
  setOptionalInt(fans, "chamber", s.chamberFan);

  root["wifi_dbm"] = s.wifiDbm;
  root["light"] = s.chamberLight;
  root["print_error"] = s.printError;

  JsonArray hms = root["hms"].to<JsonArray>();
  for (uint8_t i = 0; i < s.hmsCount; ++i) {
    hms.add(hmsCodeString(s.hms[i]));
  }

  JsonObject ams = root["ams"].to<JsonObject>();
  ams["now"] = s.trayNow;
  JsonArray units = ams["units"].to<JsonArray>();
  for (uint8_t a = 0; a < s.amsCount; ++a) {
    JsonObject unit = units.add<JsonObject>();
    unit["id"] = a;
    unit["humidity"] = s.amsHumidity[a];
    JsonArray trays = unit["trays"].to<JsonArray>();
    for (uint8_t t = 0; t < PrinterState::kTraysPerAms; ++t) {
      const AmsTray& tray = s.trays[a][t];
      JsonObject item = trays.add<JsonObject>();
      item["present"] = tray.present;
      item["type"] = tray.type;
      char color[8];
      snprintf(color, sizeof(color), "#%06lX",
               static_cast<unsigned long>(tray.color & 0xFFFFFF));
      item["color"] = color;
      item["remain"] = tray.remain;
    }
  }

  JsonArray log = root["log"].to<JsonArray>();
  const uint32_t now = millis();
  for (uint8_t i = 0; i < commentator.historyCount(); ++i) {
    const Comment& c = commentator.history(i);
    JsonObject entry = log.add<JsonObject>();
    entry["text"] = c.text;
    entry["mood"] = moodName(c.mood);
    entry["ago"] = static_cast<int>((now - c.createdAt) / 1000);
  }

  String out;
  serializeJson(doc, out);
  return out;
}
