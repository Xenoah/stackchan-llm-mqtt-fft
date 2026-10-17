#include "BambuMqttClient.h"

#include <WiFi.h>

namespace {

constexpr uint16_t kMqttPort = 8883;
constexpr char kMqttUser[] = "bblp";  // Bambu Lab LAN MQTT の固定ユーザー名

// P1S の pushall（AMS 情報込み）は 8KB を超えることがあるため余裕を持たせる。
// 4KB を超える確保は PSRAM に置かれるので内部 RAM は圧迫しない。
constexpr uint16_t kMqttBufferSize = 24 * 1024;

constexpr uint32_t kTaskStackBytes = 12 * 1024;
constexpr uint32_t kRetryMinMs = 3000;
constexpr uint32_t kRetryMaxMs = 60000;

// pushall の最小間隔（P1 系は頻繁な pushall で負荷が上がるため間引く）
constexpr uint32_t kPushAllMinIntervalMs = 20000;
// 定期的な pushall（差分の取りこぼしを補正する）
constexpr uint32_t kPushAllPeriodMs = 10UL * 60UL * 1000UL;

// Bambu は数値を文字列で送ってくることがあるため両対応で読む
int readInt(JsonVariantConst v, int fallback) {
  if (v.is<int>()) return v.as<int>();
  if (v.is<float>()) return static_cast<int>(v.as<float>());
  if (v.is<const char*>()) {
    const char* s = v.as<const char*>();
    if (s != nullptr && *s != '\0') return atoi(s);
  }
  return fallback;
}

uint32_t readUInt(JsonVariantConst v, uint32_t fallback) {
  if (v.is<uint32_t>()) return v.as<uint32_t>();
  if (v.is<int64_t>()) return static_cast<uint32_t>(v.as<int64_t>());
  if (v.is<const char*>()) {
    const char* s = v.as<const char*>();
    if (s != nullptr && *s != '\0') return strtoul(s, nullptr, 10);
  }
  return fallback;
}

float readFloat(JsonVariantConst v, float fallback) {
  if (v.is<float>() || v.is<int>()) return v.as<float>();
  if (v.is<const char*>()) {
    const char* s = v.as<const char*>();
    if (s != nullptr && *s != '\0') return atof(s);
  }
  return fallback;
}

void copyText(char* dest, size_t size, const char* src) {
  if (src == nullptr) src = "";
  strncpy(dest, src, size - 1);
  dest[size - 1] = '\0';
}

// "Cube.gcode.3mf" や "/data/Metadata/plate_1.gcode" から表示名を作る
void jobNameFrom(char* dest, size_t size, const char* src) {
  if (src == nullptr) src = "";
  const char* base = strrchr(src, '/');
  base = base ? base + 1 : src;
  copyText(dest, size, base);
  static const char* const kSuffixes[] = {".gcode.3mf", ".3mf", ".gcode"};
  for (const char* suffix : kSuffixes) {
    const size_t len = strlen(dest);
    const size_t sl = strlen(suffix);
    if (len > sl && strcasecmp(dest + len - sl, suffix) == 0) {
      dest[len - sl] = '\0';
      break;
    }
  }
}

// Bambu のファン値は "0"〜"15" の段階値。% に変換する。
int fanPercent(JsonVariantConst v) {
  const int raw = readInt(v, -1);
  if (raw < 0) return -1;
  return constrain((raw * 100 + 7) / 15, 0, 100);
}

}  // namespace

void BambuMqttClient::begin(const BambuConfig& config) {
  config_ = config;
  config_.host.trim();
  config_.serial.trim();
  config_.accessCode.trim();
  enabled_ = config_.enabled && !config_.host.isEmpty() &&
             !config_.serial.isEmpty() && !config_.accessCode.isEmpty();

  mutex_ = xSemaphoreCreateMutex();
  work_.link = enabled_ ? LinkState::WaitingWifi : LinkState::Disabled;
  shared_ = work_;
  if (!enabled_) {
    Serial.println("[bambu] monitor disabled or not configured");
    return;
  }

  snprintf(topicReport_, sizeof(topicReport_), "device/%s/report",
           config_.serial.c_str());
  snprintf(topicRequest_, sizeof(topicRequest_), "device/%s/request",
           config_.serial.c_str());
  const uint64_t chip = ESP.getEfuseMac();
  snprintf(clientId_, sizeof(clientId_), "stackchan-%06llX",
           static_cast<unsigned long long>(chip & 0xFFFFFF));

  // 必要なフィールドだけを残すフィルタ（配列は先頭要素の形が全要素に適用される）
  JsonObject p = filter_["print"].to<JsonObject>();
  static const char* const kFields[] = {
      "command", "gcode_state", "mc_percent", "mc_remaining_time",
      "layer_num", "total_layer_num", "stg_cur", "print_error", "spd_lvl",
      "subtask_name", "gcode_file", "nozzle_temper", "nozzle_target_temper",
      "bed_temper", "bed_target_temper", "chamber_temper",
      "cooling_fan_speed", "big_fan1_speed", "big_fan2_speed", "wifi_signal",
  };
  for (const char* field : kFields) {
    p[field] = true;
  }
  p["lights_report"][0]["node"] = true;
  p["lights_report"][0]["mode"] = true;
  p["hms"][0]["attr"] = true;
  p["hms"][0]["code"] = true;
  JsonObject ams = p["ams"].to<JsonObject>();
  ams["tray_now"] = true;
  ams["ams"][0]["id"] = true;
  ams["ams"][0]["humidity"] = true;
  ams["ams"][0]["tray"][0]["id"] = true;
  ams["ams"][0]["tray"][0]["tray_type"] = true;
  ams["ams"][0]["tray"][0]["tray_color"] = true;
  ams["ams"][0]["tray"][0]["remain"] = true;

  tls_.setInsecure();         // プリンタは自己署名証明書
  tls_.setTimeout(10);        // 秒
  tls_.setHandshakeTimeout(15);
  mqtt_.setServer(config_.host.c_str(), kMqttPort);
  mqtt_.setKeepAlive(30);
  mqtt_.setSocketTimeout(10);
  if (!mqtt_.setBufferSize(kMqttBufferSize)) {
    Serial.println("[bambu] MQTT buffer alloc failed");
  }
  mqtt_.setCallback([this](char* topic, uint8_t* payload, unsigned int length) {
    onMessage(topic, payload, length);
  });

  xTaskCreatePinnedToCore(taskEntry, "bambu_mqtt", kTaskStackBytes, this, 1,
                          &task_, 0);
  Serial.printf("[bambu] monitor started host=%s serial=%s\n",
                config_.host.c_str(), config_.serial.c_str());
}

bool BambuMqttClient::isEnabled() const {
  return enabled_;
}

PrinterState BambuMqttClient::snapshot() const {
  PrinterState copy;
  if (mutex_ == nullptr) return copy;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  copy = shared_;
  xSemaphoreGive(mutex_);
  return copy;
}

uint32_t BambuMqttClient::revision() const {
  return revision_.load();
}

void BambuMqttClient::requestPushAll() {
  pushAllRequested_ = true;
}

void BambuMqttClient::requestChamberLight(bool on) {
  lightRequest_ = on ? 1 : 0;
}

void BambuMqttClient::taskEntry(void* arg) {
  static_cast<BambuMqttClient*>(arg)->taskLoop();
}

void BambuMqttClient::taskLoop() {
  uint32_t retryDelayMs = kRetryMinMs;
  uint32_t nextAttemptAt = 0;

  for (;;) {
    const uint32_t now = millis();

    if (WiFi.status() != WL_CONNECTED) {
      if (mqtt_.connected()) mqtt_.disconnect();
      setLink(LinkState::WaitingWifi);
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    if (!mqtt_.connected()) {
      if (work_.link == LinkState::Online) {
        Serial.println("[bambu] connection lost");
        setLink(LinkState::Error, mqtt_.state());
        nextAttemptAt = now + kRetryMinMs;
      }
      if (static_cast<int32_t>(now - nextAttemptAt) < 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
        continue;
      }
      setLink(LinkState::Connecting);
      if (connectOnce()) {
        retryDelayMs = kRetryMinMs;
        setLink(LinkState::Online);
        publishPushAll();
      } else {
        const int err = mqtt_.state();
        Serial.printf("[bambu] connect failed state=%d, retry in %us\n", err,
                      (unsigned)(retryDelayMs / 1000));
        tls_.stop();
        setLink(LinkState::Error, err);
        nextAttemptAt = millis() + retryDelayMs;
        retryDelayMs = min(retryDelayMs * 2, kRetryMaxMs);
      }
      continue;
    }

    mqtt_.loop();

    if (pushAllRequested_.exchange(false) &&
        millis() - lastPushAllAt_ >= kPushAllMinIntervalMs) {
      publishPushAll();
    } else if (millis() - lastPushAllAt_ >= kPushAllPeriodMs) {
      publishPushAll();
    }

    const int8_t light = lightRequest_.exchange(-1);
    if (light >= 0) {
      publishChamberLight(light == 1);
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

bool BambuMqttClient::connectOnce() {
  Serial.printf("[bambu] connecting to %s:%u\n", config_.host.c_str(),
                kMqttPort);
  if (!mqtt_.connect(clientId_, kMqttUser, config_.accessCode.c_str())) {
    return false;
  }
  if (!mqtt_.subscribe(topicReport_)) {
    Serial.println("[bambu] subscribe failed");
    mqtt_.disconnect();
    return false;
  }
  Serial.println("[bambu] connected and subscribed");
  return true;
}

void BambuMqttClient::onMessage(char* topic, uint8_t* payload,
                                unsigned int length) {
  (void)topic;
  JsonDocument doc;
  const DeserializationError error =
      deserializeJson(doc, reinterpret_cast<const char*>(payload), length,
                      DeserializationOption::Filter(filter_));
  if (error) {
    Serial.printf("[bambu] JSON error: %s (len=%u)\n", error.c_str(), length);
    return;
  }

  JsonObjectConst print = doc["print"];
  if (print.isNull()) {
    return;
  }
  const char* command = print["command"] | "";
  if (strcmp(command, "push_status") != 0) {
    return;
  }

  applyPrint(print);
  work_.lastMessageAt = millis();
  work_.messageCount++;
  commit();
}

void BambuMqttClient::applyPrint(JsonObjectConst print) {
  PrinterState& s = work_;

  if (print["gcode_state"].is<const char*>()) {
    copyText(s.gcodeState, sizeof(s.gcodeState), print["gcode_state"]);
    s.phase = printPhaseFromString(s.gcodeState);
    s.synced = true;
  }
  if (!print["mc_percent"].isNull()) {
    s.percent = constrain(readInt(print["mc_percent"], s.percent), -1, 100);
  }
  if (!print["mc_remaining_time"].isNull()) {
    s.remainingMin = readInt(print["mc_remaining_time"], s.remainingMin);
  }
  if (!print["layer_num"].isNull()) {
    s.layer = readInt(print["layer_num"], s.layer);
  }
  if (!print["total_layer_num"].isNull()) {
    s.totalLayers = readInt(print["total_layer_num"], s.totalLayers);
  }
  if (!print["stg_cur"].isNull()) {
    s.stage = readInt(print["stg_cur"], s.stage);
  }
  if (!print["print_error"].isNull()) {
    s.printError = readUInt(print["print_error"], s.printError);
  }
  if (!print["spd_lvl"].isNull()) {
    const int lvl = readInt(print["spd_lvl"], 0);
    s.speed = (lvl >= 1 && lvl <= 4) ? static_cast<SpeedLevel>(lvl)
                                     : SpeedLevel::Unknown;
  }
  if (print["subtask_name"].is<const char*>() &&
      strlen(print["subtask_name"].as<const char*>()) > 0) {
    jobNameFrom(s.jobName, sizeof(s.jobName), print["subtask_name"]);
  } else if (print["gcode_file"].is<const char*>() && s.jobName[0] == '\0') {
    jobNameFrom(s.jobName, sizeof(s.jobName), print["gcode_file"]);
  }

  if (!print["nozzle_temper"].isNull()) {
    s.nozzleTemp = readFloat(print["nozzle_temper"], s.nozzleTemp);
  }
  if (!print["nozzle_target_temper"].isNull()) {
    s.nozzleTarget = readFloat(print["nozzle_target_temper"], s.nozzleTarget);
  }
  if (!print["bed_temper"].isNull()) {
    s.bedTemp = readFloat(print["bed_temper"], s.bedTemp);
  }
  if (!print["bed_target_temper"].isNull()) {
    s.bedTarget = readFloat(print["bed_target_temper"], s.bedTarget);
  }
  if (!print["chamber_temper"].isNull()) {
    s.chamberTemp = readFloat(print["chamber_temper"], s.chamberTemp);
  }

  if (!print["cooling_fan_speed"].isNull()) {
    s.partFan = fanPercent(print["cooling_fan_speed"]);
  }
  if (!print["big_fan1_speed"].isNull()) {
    s.auxFan = fanPercent(print["big_fan1_speed"]);
  }
  if (!print["big_fan2_speed"].isNull()) {
    s.chamberFan = fanPercent(print["big_fan2_speed"]);
  }
  if (!print["wifi_signal"].isNull()) {
    s.wifiDbm = readInt(print["wifi_signal"], s.wifiDbm);  // "-45dBm" → -45
  }

  JsonArrayConst lights = print["lights_report"];
  for (JsonObjectConst light : lights) {
    if (strcmp(light["node"] | "", "chamber_light") == 0) {
      s.chamberLight = strcmp(light["mode"] | "", "on") == 0 ? 1 : 0;
    }
  }

  if (print["hms"].is<JsonArrayConst>()) {
    s.hmsCount = 0;
    for (JsonObjectConst entry : print["hms"].as<JsonArrayConst>()) {
      if (s.hmsCount >= PrinterState::kMaxHms) break;
      s.hms[s.hmsCount].attr = readUInt(entry["attr"], 0);
      s.hms[s.hmsCount].code = readUInt(entry["code"], 0);
      s.hmsCount++;
    }
  }

  JsonObjectConst ams = print["ams"];
  if (!ams.isNull()) {
    if (!ams["tray_now"].isNull()) {
      const int now = readInt(ams["tray_now"], 255);
      s.trayNow = (now == 255) ? -1 : now;
    }
    if (ams["ams"].is<JsonArrayConst>()) {
      uint8_t count = 0;
      for (JsonObjectConst unit : ams["ams"].as<JsonArrayConst>()) {
        const int id = readInt(unit["id"], count);
        if (id < 0 || id >= PrinterState::kMaxAms) continue;
        count = max<uint8_t>(count, static_cast<uint8_t>(id + 1));
        s.amsHumidity[id] = readInt(unit["humidity"], s.amsHumidity[id]);
        if (!unit["tray"].is<JsonArrayConst>()) continue;
        for (JsonObjectConst tray : unit["tray"].as<JsonArrayConst>()) {
          const int tid = readInt(tray["id"], -1);
          if (tid < 0 || tid >= PrinterState::kTraysPerAms) continue;
          AmsTray& t = s.trays[id][tid];
          const char* type = tray["tray_type"] | "";
          t.present = type[0] != '\0';
          copyText(t.type, sizeof(t.type), type);
          const char* color = tray["tray_color"] | "";
          if (strlen(color) >= 6) {
            char hex[7];
            memcpy(hex, color, 6);
            hex[6] = '\0';
            t.color = strtoul(hex, nullptr, 16);
          }
          t.remain = static_cast<int8_t>(
              constrain(readInt(tray["remain"], -1), -1, 100));
        }
      }
      s.amsCount = count;
    }
  }
}

void BambuMqttClient::publishPushAll() {
  JsonDocument doc;
  JsonObject pushing = doc["pushing"].to<JsonObject>();
  pushing["sequence_id"] = String(++sequenceId_);
  pushing["command"] = "pushall";
  pushing["version"] = 1;
  pushing["push_target"] = 1;
  if (publishJson(doc)) {
    Serial.println("[bambu] pushall sent");
  }
  lastPushAllAt_ = millis();
}

void BambuMqttClient::publishChamberLight(bool on) {
  JsonDocument doc;
  JsonObject system = doc["system"].to<JsonObject>();
  system["sequence_id"] = String(++sequenceId_);
  system["command"] = "ledctrl";
  system["led_node"] = "chamber_light";
  system["led_mode"] = on ? "on" : "off";
  system["led_on_time"] = 500;
  system["led_off_time"] = 500;
  system["loop_times"] = 0;
  system["interval_time"] = 0;
  publishJson(doc);
}

bool BambuMqttClient::publishJson(JsonDocument& doc) {
  String payload;
  serializeJson(doc, payload);
  // retain=false: コマンドをブローカーに保持させない（再接続時の誤再送防止）
  return mqtt_.publish(topicRequest_, payload.c_str(), false);
}

void BambuMqttClient::setLink(LinkState link, int errorCode) {
  if (work_.link == link && work_.mqttErrorCode == errorCode) {
    return;
  }
  work_.link = link;
  work_.mqttErrorCode = errorCode;
  commit();
}

void BambuMqttClient::commit() {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  shared_ = work_;
  xSemaphoreGive(mutex_);
  revision_++;
}

uint32_t BambuMqttClient::stackFree() const {
  return task_ ? uxTaskGetStackHighWaterMark(task_) : 0;
}
