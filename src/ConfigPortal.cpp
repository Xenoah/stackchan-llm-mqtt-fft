#include "ConfigPortal.h"

#include "Version.h"
#include "WebPages.h"

namespace {

// HTMLの特殊文字（<>&"'）をエンティティにエスケープする。
// ユーザ入力をHTMLに埋め込む際のXSS対策として使用する。
String htmlEscape(const String& value) {
  String escaped;
  escaped.reserve(value.length() + 16);
  for (size_t i = 0; i < value.length(); ++i) {
    switch (value[i]) {
      case '&':
        escaped += F("&amp;");
        break;
      case '<':
        escaped += F("&lt;");
        break;
      case '>':
        escaped += F("&gt;");
        break;
      case '"':
        escaped += F("&quot;");
        break;
      case '\'':
        escaped += F("&#39;");
        break;
      default:
        escaped += value[i];
        break;
    }
  }
  return escaped;
}

// TTSエンジン種別の文字列を正規化する。
// 不正な値が来た場合はデフォルトの "voicevox_compatible" を返す。
String sanitizedTtsEngineType(const String& value) {
  if (value == "simple_wav" || value == "builtin") return value;
  return "voicevox_compatible";
}

// 文字列をJSON文字列値としてエスケープする（"\ と制御文字を処理）。
String jsonEscape(const String& value) {
  String out;
  out.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    switch (c) {
      case '"':  out += F("\\\""); break;
      case '\\': out += F("\\\\"); break;
      case '\n': out += F("\\n"); break;
      case '\r': out += F("\\r"); break;
      case '\t': out += F("\\t"); break;
      case '<':  out += F("\\u003c"); break;  // </script> 対策
      default:   out += c; break;
    }
  }
  return out;
}

// <option> タグの selected 属性を返す（現在の値と一致する場合のみ付与）
String selectedAttribute(const String& value, const char* option) {
  if (value == option) {
    return " selected";
  }
  return "";
}

// チェックボックス（label.check 形式）を生成する
String checkbox(const char* name, bool checked, const char* label) {
  String h = F("<label class='check'><input type='checkbox' value='1' name='");
  h += name;
  h += '\'';
  if (checked) h += F(" checked");
  h += '>';
  h += label;
  h += F("</label>");
  return h;
}

// 数値の <option> 群を生成する（values と labels は同数）
String numberOptions(int current, const int* values, const char* const* labels,
                     size_t count) {
  String h;
  for (size_t i = 0; i < count; ++i) {
    h += "<option value='" + String(values[i]) + "'";
    if (values[i] == current) h += F(" selected");
    h += ">";
    h += labels[i];
    h += F("</option>");
  }
  return h;
}

bool isTruthy(const String& value) {
  return value == "1" || value == "true" || value == "on" || value == "yes";
}

}  // namespace

bool ConfigPortal::begin() {
  load();           // NVSから設定を読み込む
  registerRoutes(); // Webサーバのルートを登録する

  // 保存済みSSIDがあればWiFi接続を試みる
  if (!config_.wifiSsid.isEmpty() && connectWifi()) {
    server_.begin(); // 接続成功: STAモードでWebサーバ起動
    return true;
  }

  // 接続失敗: セットアップ用APを起動する
  startPortal();
  return false;
}

// WebサーバのHTTPリクエストを処理する（毎フレーム呼ぶ必要がある）
void ConfigPortal::update() {
  server_.handleClient();
}

bool ConfigPortal::isPortalActive() const {
  return portalActive_;
}

bool ConfigPortal::isConnected() const {
  return WiFi.status() == WL_CONNECTED;
}

const AppConfig& ConfigPortal::config() const {
  return config_;
}

// APモード時はsoftAPのIPを、STA接続時はDHCPで取得したIPを返す
IPAddress ConfigPortal::localIp() const {
  return portalActive_ ? WiFi.softAPIP() : WiFi.localIP();
}

String ConfigPortal::accessPointName() const {
  return accessPointName_;
}

void ConfigPortal::setRuntimeStatus(const RuntimeStatus& status) {
  runtimeStatus_ = status;
}

void ConfigPortal::setSpeakRequestHandler(
    std::function<bool(const String&)> handler) {
  speakRequestFn_ = handler;
}

void ConfigPortal::setSpeakingProbe(std::function<bool()> probe) {
  speakingProbe_ = probe;
}

void ConfigPortal::setPrinterApi(const PrinterWebApi& api) {
  printerApi_ = api;
}

void ConfigPortal::setCommentaryVoice(bool enabled) {
  if (config_.commentaryVoice == enabled) return;
  config_.commentaryVoice = enabled;
  preferences_.begin("stackchan", false);
  preferences_.putBool("cm_voice", enabled);
  preferences_.end();
}

bool ConfigPortal::testWifi(const String& ssid, const String& password,
                           uint32_t timeoutMs,
                           const std::function<void()>& service) {
  // セットアップ AP・設定用 AP は残したまま STA だけつなぎ替える
  WiFi.mode(portalActive_ || settingsApActive_ ? WIFI_AP_STA : WIFI_STA);
  WiFi.disconnect(false, false);
  delay(100);
  WiFi.begin(ssid.c_str(), password.c_str());
  const uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < timeoutMs) {
    if (service) service();
    delay(20);
  }
  const bool ok = WiFi.status() == WL_CONNECTED;
  if (!ok) {
    WiFi.disconnect(false, false);
    // 元の Wi-Fi に戻す（セットアップ AP 中は元の接続が無いのでそのまま）
    if (!portalActive_ && !config_.wifiSsid.isEmpty()) {
      delay(100);
      WiFi.begin(config_.wifiSsid.c_str(), config_.wifiPassword.c_str());
    }
  }
  Serial.printf("[setup] wifi test %s: %s\n", ssid.c_str(), ok ? "ok" : "failed");
  return ok;
}

void ConfigPortal::saveWifi(const String& ssid, const String& password) {
  config_.wifiSsid = ssid;
  config_.wifiPassword = password;
  save();
}

void ConfigPortal::saveBambu(bool enabled, const String& host,
                             const String& serial, const String& accessCode,
                             bool autoMode) {
  config_.bambuEnabled = enabled;
  config_.bambuHost = host;
  config_.bambuHost.trim();
  config_.bambuSerial = serial;
  config_.bambuSerial.trim();
  config_.bambuSerial.toUpperCase();
  if (!accessCode.isEmpty()) {
    config_.bambuAccessCode = accessCode;
    config_.bambuAccessCode.trim();
  }
  config_.autoPrinterMode = autoMode;
  save();
}

// SETTINGSメニューを開いたときに呼ぶ。
// 既存のWiFi接続（STA）を切断せず、AP_STAモードで追加APを起動する。
void ConfigPortal::startSettingsAp() {
  if (settingsApActive_ || portalActive_) return;
  ensureAccessPointName();
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(accessPointName_.c_str(), "stackchan");
  settingsApActive_ = true;
}

// SETTINGSメニューを閉じたときに呼ぶ。APを停止してSTAモードに戻す。
void ConfigPortal::stopSettingsAp() {
  if (!settingsApActive_) return;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  settingsApActive_ = false;
}

bool ConfigPortal::isSettingsApActive() const {
  return settingsApActive_;
}

IPAddress ConfigPortal::settingsApIp() const {
  return WiFi.softAPIP();
}

// NVSのnamespace "stackchan" から設定を読み込む。
// 古いキー名（vv_host, vv_port 等）との後方互換性も維持する。
void ConfigPortal::load() {
  preferences_.begin("stackchan", true);
  config_.wifiSsid = preferences_.getString("ssid", "");
  config_.wifiPassword = preferences_.getString("password", "");
  config_.ttsHost = preferences_.getString(
      "tts_host", preferences_.getString("vv_host", config_.ttsHost));
  config_.ttsPort = preferences_.getUShort(
      "tts_port", preferences_.getUShort("vv_port", config_.ttsPort));
  config_.ttsSpeaker = preferences_.getString(
      "tts_speaker",
      String(preferences_.getInt("vv_speaker", config_.ttsSpeaker.toInt())));
  config_.ttsEngineType = sanitizedTtsEngineType(
      preferences_.getString("tts_engine", config_.ttsEngineType));
  config_.speechText =
      preferences_.getString("speech", config_.speechText);
  config_.cameraGaze = preferences_.getBool("cam_gaze", config_.cameraGaze);
  config_.gamingRgb = preferences_.getBool("gaming_rgb", config_.gamingRgb);

  config_.bambuEnabled = preferences_.getBool("bb_on", config_.bambuEnabled);
  // 未保存のキーを getString すると Preferences がエラーログを出すので、存在を確かめてから読む
  auto readString = [this](const char* key, const String& fallback) {
    return preferences_.isKey(key) ? preferences_.getString(key, fallback)
                                   : fallback;
  };
  config_.bambuHost = readString("bb_host", "");
  config_.bambuSerial = readString("bb_serial", "");
  config_.bambuAccessCode = readString("bb_code", "");
  config_.autoPrinterMode =
      preferences_.getBool("auto_mqtt", config_.autoPrinterMode);
  config_.commentaryVoice =
      preferences_.getBool("cm_voice", config_.commentaryVoice);
  config_.commentaryStep =
      preferences_.getUChar("cm_step", config_.commentaryStep);
  config_.commentaryPeriodMin =
      preferences_.getUShort("cm_period", config_.commentaryPeriodMin);
  config_.commentaryStages =
      preferences_.getBool("cm_stages", config_.commentaryStages);
  config_.commentaryTemps =
      preferences_.getBool("cm_temps", config_.commentaryTemps);
  config_.quietEnabled = preferences_.getBool("quiet_on", config_.quietEnabled);
  config_.quietFrom = preferences_.getUChar("quiet_from", config_.quietFrom);
  config_.quietTo = preferences_.getUChar("quiet_to", config_.quietTo);
  config_.speakerVolume = preferences_.getUChar("volume", config_.speakerVolume);
  config_.printerHud = preferences_.getBool("hud", config_.printerHud);
  config_.ledProgress = preferences_.getBool("led_prog", config_.ledProgress);
  config_.timezone = readString("tz", config_.timezone);
  preferences_.end();
}

// 現在の設定をNVSに保存する（保存後は再起動が必要）
void ConfigPortal::save() {
  preferences_.begin("stackchan", false);
  preferences_.putString("ssid", config_.wifiSsid);
  preferences_.putString("password", config_.wifiPassword);
  preferences_.putString("tts_host", config_.ttsHost);
  preferences_.putUShort("tts_port", config_.ttsPort);
  preferences_.putString("tts_speaker", config_.ttsSpeaker);
  preferences_.putString("tts_engine", config_.ttsEngineType);
  preferences_.putString("speech", config_.speechText);
  preferences_.putBool("cam_gaze", config_.cameraGaze);
  preferences_.putBool("gaming_rgb", config_.gamingRgb);

  preferences_.putBool("bb_on", config_.bambuEnabled);
  preferences_.putString("bb_host", config_.bambuHost);
  preferences_.putString("bb_serial", config_.bambuSerial);
  preferences_.putString("bb_code", config_.bambuAccessCode);
  preferences_.putBool("auto_mqtt", config_.autoPrinterMode);
  preferences_.putBool("cm_voice", config_.commentaryVoice);
  preferences_.putUChar("cm_step", config_.commentaryStep);
  preferences_.putUShort("cm_period", config_.commentaryPeriodMin);
  preferences_.putBool("cm_stages", config_.commentaryStages);
  preferences_.putBool("cm_temps", config_.commentaryTemps);
  preferences_.putBool("quiet_on", config_.quietEnabled);
  preferences_.putUChar("quiet_from", config_.quietFrom);
  preferences_.putUChar("quiet_to", config_.quietTo);
  preferences_.putUChar("volume", config_.speakerVolume);
  preferences_.putBool("hud", config_.printerHud);
  preferences_.putBool("led_prog", config_.ledProgress);
  preferences_.putString("tz", config_.timezone);
  preferences_.end();
}

// 保存済みSSID/パスワードでWiFiに接続する（最大15秒待機）
bool ConfigPortal::connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(config_.wifiSsid.c_str(), config_.wifiPassword.c_str());

  const uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < 15000) {
    delay(100);
  }
  return WiFi.status() == WL_CONNECTED;
}

// APのSSID名をMACアドレス末尾3バイトで生成する（未設定の場合のみ）
void ConfigPortal::ensureAccessPointName() {
  if (!accessPointName_.isEmpty()) return;
  const uint64_t chipId = ESP.getEfuseMac();
  char suffix[7];
  snprintf(suffix, sizeof(suffix), "%06llX",
           static_cast<unsigned long long>(chipId & 0xFFFFFF));
  accessPointName_ = "StackChan-Setup-" + String(suffix);
}

// WiFi接続失敗時のセットアップ用APを起動する
void ConfigPortal::startPortal() {
  portalActive_ = true;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP_STA);
  ensureAccessPointName();
  WiFi.softAP(accessPointName_.c_str(), "stackchan");
  server_.begin();
}

// WebサーバのURLルートを登録する
void ConfigPortal::registerRoutes() {
  // GET / → プリンタ ダッシュボード（セットアップAP中は設定ページ）
  server_.on("/", HTTP_GET, [this]() {
    if (portalActive_) {
      server_.send(200, "text/html; charset=utf-8", pageHtml());
      return;
    }
    server_.send_P(200, "text/html; charset=utf-8", kDashboardHtml);
  });

  // GET /settings → 設定ページ（現在値を事前入力済み）
  server_.on("/settings", HTTP_GET, [this]() {
    server_.send(200, "text/html; charset=utf-8", pageHtml());
  });

  // GET /app.css → 共通スタイル（ブラウザにキャッシュさせる）
  server_.on("/app.css", HTTP_GET, [this]() {
    server_.sendHeader("Cache-Control", "max-age=86400");
    server_.send_P(200, "text/css; charset=utf-8", kAppCss);
  });

  // POST /save → 設定を保存して再起動
  server_.on("/save", HTTP_POST, [this]() {
    const String manualSsid = server_.arg("manual_ssid");
    config_.wifiSsid =
        manualSsid.isEmpty() ? server_.arg("ssid") : manualSsid;

    // パスワードは送信された場合のみ更新（空欄なら既存値を維持）
    const String submittedPassword = server_.arg("password");
    if (!submittedPassword.isEmpty()) {
      config_.wifiPassword = submittedPassword;
    }

    config_.ttsHost = server_.arg("tts_host");
    config_.ttsHost.trim();
    config_.ttsPort =
        constrain(server_.arg("tts_port").toInt(), 1, 65535);
    config_.ttsSpeaker = server_.arg("speaker");
    config_.ttsSpeaker.trim();
    if (config_.ttsSpeaker.isEmpty()) {
      config_.ttsSpeaker = "3";
    }
    config_.ttsEngineType =
        sanitizedTtsEngineType(server_.arg("tts_engine"));
    config_.speechText = server_.arg("speech");
    // チェックボックスは未チェック時にPOSTされないため、存在で判定する
    config_.cameraGaze = server_.hasArg("camera_gaze");
    config_.gamingRgb = server_.hasArg("gaming_rgb");

    // プリンタ
    config_.bambuEnabled = server_.hasArg("bambu_enabled");
    config_.autoPrinterMode = server_.hasArg("auto_mqtt");
    config_.bambuHost = server_.arg("bambu_host");
    config_.bambuHost.trim();
    config_.bambuSerial = server_.arg("bambu_serial");
    config_.bambuSerial.trim();
    config_.bambuSerial.toUpperCase();
    String code = server_.arg("bambu_code");
    code.trim();
    if (!code.isEmpty()) {
      config_.bambuAccessCode = code;
    }

    // 実況・表示
    config_.commentaryVoice = server_.hasArg("cm_voice");
    config_.commentaryStep =
        constrain(server_.arg("cm_step").toInt(), 0, 50);
    config_.commentaryPeriodMin =
        constrain(server_.arg("cm_period").toInt(), 0, 240);
    config_.commentaryStages = server_.hasArg("cm_stages");
    config_.commentaryTemps = server_.hasArg("cm_temps");
    config_.quietEnabled = server_.hasArg("quiet_on");
    config_.quietFrom = constrain(server_.arg("quiet_from").toInt(), 0, 23);
    config_.quietTo = constrain(server_.arg("quiet_to").toInt(), 0, 23);
    if (server_.hasArg("volume")) {
      config_.speakerVolume = constrain(server_.arg("volume").toInt(), 0, 255);
    }
    config_.printerHud = server_.hasArg("hud");
    config_.ledProgress = server_.hasArg("led_progress");
    String tz = server_.arg("tz");
    tz.trim();
    config_.timezone = tz.isEmpty() ? String("JST-9") : tz;
    save();

    server_.send(200, "text/html; charset=utf-8",
                 pageHtml("保存しました。StackChan を再起動しています…"));
    delay(800);
    ESP.restart();
  });

  // GET /status → システム状態ページ（5秒自動更新）
  server_.on("/status", HTTP_GET, [this]() {
    server_.send(200, "text/html; charset=utf-8", statusHtml());
  });

  // POST /api/speak → Gateway からの発話指示（ブラウザ送信→自動発話）。
  // 任意の text フォーム引数、または text/plain 本文で発話テキストを指定できる。
  // 話し中なら busy を返す。受け付けたら ok:true（実際の再生は loop() 側）。
  server_.on("/api/speak", HTTP_POST, [this]() {
    String text;
    if (server_.hasArg("text")) {
      text = server_.arg("text");
    } else if (server_.hasArg("plain")) {
      text = server_.arg("plain"); // text/plain 本文
    }
    const bool accepted = speakRequestFn_ ? speakRequestFn_(text) : false;
    if (accepted) {
      server_.send(200, "application/json", "{\"ok\":true}");
    } else {
      server_.send(200, "application/json",
                   "{\"ok\":false,\"error\":\"busy\"}");
    }
  });

  // GET /api/status → デバイス状態をJSONで返す（Gateway の疎通確認用）
  server_.on("/api/status", HTTP_GET, [this]() {
    server_.send(200, "application/json", apiStatusJson());
  });

  // --- プリンタ ダッシュボード API ---
  server_.on("/api/printer", HTTP_GET, [this]() {
    server_.sendHeader("Cache-Control", "no-store");
    if (printerApi_.stateJson) {
      server_.send(200, "application/json", printerApi_.stateJson());
    } else {
      server_.send(200, "application/json",
                   "{\"enabled\":false,\"link\":\"disabled\"}");
    }
  });

  auto ok = [this]() {
    server_.send(200, "application/json", "{\"ok\":true}");
  };

  server_.on("/api/printer/report", HTTP_POST, [this, ok]() {
    if (printerApi_.report) printerApi_.report();
    ok();
  });

  server_.on("/api/printer/refresh", HTTP_POST, [this, ok]() {
    if (printerApi_.refresh) printerApi_.refresh();
    ok();
  });

  server_.on("/api/printer/light", HTTP_POST, [this, ok]() {
    if (printerApi_.light) printerApi_.light(isTruthy(server_.arg("on")));
    ok();
  });

  server_.on("/api/printer/voice", HTTP_POST, [this, ok]() {
    if (printerApi_.voice) printerApi_.voice(isTruthy(server_.arg("on")));
    ok();
  });

  // POST /api/mode → モード切り替え（mode=mqtt / llm / level）
  server_.on("/api/mode", HTTP_POST, [this]() {
    const String mode = server_.arg("mode");
    const bool valid = mode == "mqtt" || mode == "llm" || mode == "level";
    if (valid && printerApi_.mode) printerApi_.mode(mode);
    server_.send(200, "application/json",
                 valid ? "{\"ok\":true}" : "{\"ok\":false}");
  });

  server_.on("/api/printer/say", HTTP_POST, [this]() {
    String text = server_.arg("text");
    text.trim();
    if (text.length() > 360) text = text.substring(0, 360);
    const bool accepted =
        !text.isEmpty() && printerApi_.say && printerApi_.say(text);
    server_.send(200, "application/json",
                 accepted ? "{\"ok\":true}" : "{\"ok\":false}");
  });

  server_.onNotFound([this]() {
    server_.send(404, "text/plain; charset=utf-8", "Not found");
  });
}

String ConfigPortal::pageHead(const char* title, const char* active,
                              bool autoRefresh) {
  String h;
  h.reserve(900);
  h += F("<!doctype html><html lang='ja'><head><meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>");
  if (autoRefresh) {
    h += F("<meta http-equiv='refresh' content='5'>");
  }
  h += F("<title>");
  h += title;
  h += F("</title><link rel='stylesheet' href='/app.css'></head><body>"
         "<header><div class='bar'><span class='brand'>StackChan × Bambu</span>"
         "<nav>");
  struct Item {
    const char* key;
    const char* href;
    const char* label;
  };
  static const Item kItems[] = {
      {"printer", "/", "プリンター"},
      {"settings", "/settings", "設定"},
      {"status", "/status", "状態"},
  };
  for (const Item& item : kItems) {
    // セットアップAP中はダッシュボードが無いので「プリンター」を出さない
    if (portalActive_ && strcmp(item.key, "printer") == 0) continue;
    h += F("<a href='");
    h += item.href;
    h += '\'';
    if (strcmp(item.key, active) == 0) h += F(" class='on'");
    h += '>';
    h += item.label;
    h += F("</a>");
  }
  h += F("</nav></div></header><main>");
  return h;
}

// 設定WebページのHTMLを生成する。
// 現在保存されている値をフォームに事前入力する。
// パスワード・アクセスコードは値を表示せず、設定済みかどうかのみを示す。
String ConfigPortal::pageHtml(const String& message) {
  String html;
  html.reserve(14000);
  html += pageHead("StackChan 設定", "settings");
  html += F("<h1>設定</h1>");

  if (!message.isEmpty()) {
    html += "<div class='card ok'><strong>" + htmlEscape(message) +
            "</strong></div>";
  }
  if (portalActive_) {
    html += F("<div class='card warn'>セットアップモードです。Wi-Fi を設定して"
              "保存すると再起動し、家の Wi-Fi に接続します。</div>");
  }

  html += F("<form method='post' action='/save'>");

  // --- プリンタ ---
  html += F("<section class='card' id='printer'><h3>🖨 Bambu Lab プリンター</h3>");
  html += checkbox("bambu_enabled", config_.bambuEnabled,
                   "プリンターを監視して実況する");
  html += checkbox("auto_mqtt", config_.autoPrinterMode,
                   "印刷が始まったら自動で MQTT モード（プリンター実況）にする");
  html += F("<div class='grid2'><label>IP アドレス<input name='bambu_host' "
            "inputmode='decimal' placeholder='192.168.1.50' value='");
  html += htmlEscape(config_.bambuHost);
  html += F("'></label><label>シリアル番号<input name='bambu_serial' "
            "autocapitalize='characters' placeholder='01P00A123456789' value='");
  html += htmlEscape(config_.bambuSerial);
  html += F("'></label></div><label>アクセスコード");
  if (!config_.bambuAccessCode.isEmpty()) {
    html += F("<span class='hint'> — 設定済み（変更するときだけ入力）</span>");
  } else {
    html += F("<span class='hint'> — 未設定</span>");
  }
  html += F("<input type='password' name='bambu_code' autocomplete='off' "
            "placeholder='8桁のアクセスコード'></label>"
            "<p class='hint'>プリンター本体の「設定 → WLAN」で IP アドレスと"
            "アクセスコード、「設定 → デバイス」でシリアル番号を確認できます。"
            "つながらないときは値を再確認し、ファームウェアによっては"
            "「LANのみモード」と「開発者モード」を有効にしてください。</p></section>");

  // --- 実況 ---
  html += F("<section class='card'><h3>🗣 実況</h3>");
  html += checkbox("cm_voice", config_.commentaryVoice,
                   "実況を声で喋る（OFF でも字幕と表情は出ます）");
  {
    static const int kStepValues[] = {0, 5, 10, 20, 25};
    static const char* const kStepLabels[] = {"しない", "5% ごと", "10% ごと",
                                              "20% ごと", "25% ごと"};
    static const int kPeriodValues[] = {0, 5, 10, 15, 30, 60};
    static const char* const kPeriodLabels[] = {
        "しない", "5 分", "10 分", "15 分", "30 分", "60 分"};
    html += F("<div class='grid2'><label>進捗の実況<select name='cm_step'>");
    html += numberOptions(config_.commentaryStep, kStepValues, kStepLabels, 5);
    html += F("</select></label><label>無言が続いたら状況報告<select name='cm_period'>");
    html += numberOptions(config_.commentaryPeriodMin, kPeriodValues,
                          kPeriodLabels, 6);
    html += F("</select></label></div>");
  }
  html += checkbox("cm_stages", config_.commentaryStages,
                   "準備工程（レベリング・加熱・ノズル清掃など）を実況");
  html += checkbox("cm_temps", config_.commentaryTemps,
                   "ノズル・ベッドが目標温度になったら実況");
  html += checkbox("quiet_on", config_.quietEnabled,
                   "夜間は重要な実況（完了・失敗・エラー）だけ声に出す");
  {
    static const int kHourValues[] = {0,  1,  2,  3,  4,  5,  6,  7,
                                      8,  9,  10, 11, 12, 13, 14, 15,
                                      16, 17, 18, 19, 20, 21, 22, 23};
    static const char* const kHourLabels[] = {
        "0時",  "1時",  "2時",  "3時",  "4時",  "5時",  "6時",  "7時",
        "8時",  "9時",  "10時", "11時", "12時", "13時", "14時", "15時",
        "16時", "17時", "18時", "19時", "20時", "21時", "22時", "23時"};
    html += F("<div class='grid2'><label>夜間の開始<select name='quiet_from'>");
    html += numberOptions(config_.quietFrom, kHourValues, kHourLabels, 24);
    html += F("</select></label><label>夜間の終了<select name='quiet_to'>");
    html += numberOptions(config_.quietTo, kHourValues, kHourLabels, 24);
    html += F("</select></label></div>");
  }
  html += F("<label>スピーカー音量 <span class='hint' id='volv'>");
  html += String(config_.speakerVolume);
  html += F("</span><input type='range' name='volume' min='0' max='255' "
            "oninput=\"document.getElementById('volv').textContent=this.value\" value='");
  html += String(config_.speakerVolume);
  html += F("'></label>");
  html += F("<p class='hint'>開始・一時停止・完了・失敗・HMS エラーは常に実況します。"
            "頭をタップすると今の状況を話します。</p></section>");

  // --- 表示 ---
  html += F("<section class='card'><h3>🖥 表示</h3>");
  html += checkbox("hud", config_.printerHud,
                   "顔の上にプリンター HUD（進捗・温度・字幕）を表示");
  html += checkbox("led_progress", config_.ledProgress,
                   "印刷中は本体 LED で進捗を表示");
  html += checkbox("gaming_rgb", config_.gamingRgb,
                   "Gaming RGB（顔と LED を虹色に循環）");
  html += checkbox("camera_gaze", config_.cameraGaze,
                   "カメラ目線（明るい方を見る）");
  html += F("<p class='hint'>CoreS3 ではカメラとタッチ画面が内部 I2C を共有します。"
            "タッチが効かなくなったらカメラ目線を OFF にしてください。</p>"
            "<label>タイムゾーン（POSIX TZ）<input name='tz' value='");
  html += htmlEscape(config_.timezone);
  html += F("' placeholder='JST-9'></label><p class='hint'>完成予定時刻の表示に"
            "使います。日本は JST-9。</p></section>");

  // --- Wi-Fi ---
  html += F("<section class='card'><h3>📶 Wi-Fi</h3><label>SSID<select name='ssid'>");
  html += wifiOptionsHtml();
  html += F("</select></label><p class='hint'><a href='/settings'>周辺の Wi-Fi を"
            "再スキャン</a></p><label>SSID を手入力（非公開ネットワーク用）"
            "<input name='manual_ssid' placeholder='入力するとこちらを優先'></label>"
            "<label>パスワード");
  if (!config_.wifiPassword.isEmpty()) {
    html += F("<span class='hint'> — 設定済み（変更するときだけ入力）</span>");
  } else {
    html += F("<span class='hint'> — 未設定</span>");
  }
  html += F("<input type='password' name='password' placeholder='変更するときだけ入力'>"
            "</label></section>");

  // --- TTS ---
  html += F("<section class='card'><h3>🔊 音声合成（TTS）</h3>"
            "<label>エンジン<select name='tts_engine'>");
  html += "<option value='voicevox_compatible'" +
          selectedAttribute(config_.ttsEngineType, "voicevox_compatible") +
          ">VOICEVOX 互換（VOICEVOX / AivisSpeech）</option>";
  html += "<option value='simple_wav'" +
          selectedAttribute(config_.ttsEngineType, "simple_wav") +
          ">simple_wav（Android Gateway）</option>";
  html += "<option value='builtin'" +
          selectedAttribute(config_.ttsEngineType, "builtin") +
          ">内蔵ボイス（本体だけで喋る・オフライン）</option>";
  html += F("</select></label><p class='hint'>");
  if (runtimeStatus_.voiceClips > 0) {
    html += "内蔵ボイス: " + String(runtimeStatus_.voiceClips) + " クリップ書き込み済み" +
            String(runtimeStatus_.voiceFreeText
                       ? "（日本語・英語のどんな文章も読めます）"
                       : "（実況の文章だけ。どんな文章も読むにはボイスパックを作り直して書き込む）") +
            "。サーバーを選んでいても、つながらないときは自動で内蔵ボイスに切り替わります。";
  } else {
    html += F("内蔵ボイスは未書き込みです（tools/make_voice_pack.py → "
              "pio run -t uploadfs）。");
  }
  html += F("</p><div class='grid2'><label>ホスト<input name='tts_host' value='");
  html += htmlEscape(config_.ttsHost);
  html += F("'></label><label>ポート<input type='number' name='tts_port' "
            "min='1' max='65535' value='");
  html += String(config_.ttsPort);
  html += F("'></label></div><label>話者 / スタイル ID（ずんだもん ノーマル = 3）"
            "<input name='speaker' value='");
  html += htmlEscape(config_.ttsSpeaker);
  html += F("'></label><label>A ボタンで話す文章"
            "<textarea name='speech' rows='3'>");
  html += htmlEscape(config_.speechText);
  html += F("</textarea></label><p class='hint'>LOCAL LLM モードでは頭タップでも"
            "この文章を話します。MQTT モードでは頭タップでプリンターの状況を話します"
            "（A ボタンはいつもこの文章）。</p></section>");

  html += F("<div class='save'><button type='submit'>保存して再起動</button></div>"
            "</form>");

  // --- Local LLM Chat（Android Gateway の /ask をブラウザから直接叩く）---
  html += chatHtml();

  html += F("</main></body></html>");
  return html;
}

// Local LLM Chat カードと、Gateway /ask を呼ぶフロントエンド JS を生成する。
// TTS Host/Port は保存済みの設定値をそのまま JS 文字列/数値として埋め込む。
String ConfigPortal::chatHtml() {
  String html;
  html.reserve(2200);

  html += F("<section class='card'><h3>💬 Local LLM Chat（Android Gateway）</h3>"
            "<textarea id='chatText' rows='3'>");
  html += htmlEscape(F("自己紹介して"));
  html += F("</textarea>"
            "<p><button type='button' id='chatSend' class='ghost' "
            "onclick='askLlm()'>LLMで回答して喋る</button></p>"
            "<p id='chatStatus' class='hint'></p>"
            "<pre id='chatAnswer' style='display:none'></pre>"
            "</section>");

  // Gateway のベースURL（保存済みの TTS Host/Port）を JS に埋め込む
  html += F("<script>var GW_HOST=\"");
  html += jsonEscape(config_.ttsHost);
  html += F("\";var GW_PORT=");
  html += String(config_.ttsPort);
  html += F(";\n"
           "function askLlm(){"
           "var t=document.getElementById('chatText').value;"
           "var st=document.getElementById('chatStatus');"
           "var pre=document.getElementById('chatAnswer');"
           "var btn=document.getElementById('chatSend');"
           "if(!GW_HOST){st.textContent='TTS Host is not set';return;}"
           "st.textContent='thinking...';st.className='hint';"
           "btn.disabled=true;"
           "var url='http://'+GW_HOST+':'+GW_PORT+'/ask';"
           "fetch(url,{method:'POST',"
           "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
           "body:'text='+encodeURIComponent(t)})"
           ".then(function(r){if(!r.ok)throw new Error('HTTP '+r.status);return r.json();})"
           ".then(function(d){"
           "var out='';"
           "if(d.question!=null)out+='Q: '+d.question+'\\n\\n';"
           "if(d.answer!=null)out+='A: '+d.answer;"
           "if(d.stackchan!=null)out+='\\n\\n[stackchan] '+"
           "(typeof d.stackchan==='object'?JSON.stringify(d.stackchan):d.stackchan);"
           "pre.textContent=out||JSON.stringify(d);"
           "pre.style.display='block';"
           "st.textContent='done';st.className='ok';"
           "})"
           ".catch(function(e){"
           "st.textContent='fetch error: '+e.message;st.className='err';"
           "})"
           ".finally(function(){btn.disabled=false;});"
           "}\n"
           "</script>");
  return html;
}

// システム状態ページのHTMLを生成する。
// WiFi状態・TTS設定・システム情報・キャリブレーション状態を表示する。
// 5秒ごとに自動更新する。
String ConfigPortal::statusHtml() {
  String html;
  html.reserve(5000);
  html += pageHead("StackChan 状態", "status", true);
  html += F("<h1>状態</h1>");

  // --- Wi-Fi 状態 ---
  html += F("<section class='card'><h3>📶 Wi-Fi</h3><table>");
  if (isConnected()) {
    const int32_t rssi = WiFi.RSSI();
    const char* strength =
        rssi >= -55 ? "強" : (rssi >= -70 ? "中" : "弱");
    html += F("<tr><td>状態</td><td class='ok'>接続中</td></tr>");
    html += "<tr><td>SSID</td><td>" + htmlEscape(WiFi.SSID()) + "</td></tr>";
    html += "<tr><td>IP アドレス</td><td>" +
            WiFi.localIP().toString() + "</td></tr>";
    html += "<tr><td>電波</td><td>" + String(rssi) + " dBm（" +
            String(strength) + "）</td></tr>";
    html += "<tr><td>ゲートウェイ</td><td>" +
            WiFi.gatewayIP().toString() + "</td></tr>";
  } else if (portalActive_) {
    html += F("<tr><td>状態</td><td class='warn'>セットアップ AP（未接続）</td></tr>");
    html += "<tr><td>AP 名</td><td>" + htmlEscape(accessPointName_) + "</td></tr>";
    html += "<tr><td>AP IP</td><td>" + WiFi.softAPIP().toString() + "</td></tr>";
    html += F("<tr><td>パスワード</td><td>stackchan</td></tr>");
  } else {
    html += F("<tr><td>状態</td><td class='err'>未接続</td></tr>");
    if (!config_.wifiSsid.isEmpty()) {
      html += "<tr><td>保存済み SSID</td><td>" +
              htmlEscape(config_.wifiSsid) + "</td></tr>";
    }
  }

  // Settings AP が起動中ならその情報も表示する
  if (settingsApActive_) {
    html += "<tr><td>設定用 AP</td><td class='ok'>" +
            htmlEscape(accessPointName_) + "<br><span class='hint'>" +
            WiFi.softAPIP().toString() + " / pass: stackchan</span></td></tr>";
  }
  html += F("</table></section>");

  // --- プリンタ ---
  html += F("<section class='card'><h3>🖨 プリンター</h3><table>");
  html += String("<tr><td>監視</td><td class='") +
          (config_.bambuEnabled ? "ok'>ON" : "warn'>OFF") + "</td></tr>";
  html += "<tr><td>IP アドレス</td><td>" +
          htmlEscape(config_.bambuHost.isEmpty() ? String("-")
                                                 : config_.bambuHost) +
          "</td></tr>";
  html += "<tr><td>シリアル</td><td>" +
          htmlEscape(config_.bambuSerial.isEmpty() ? String("-")
                                                   : config_.bambuSerial) +
          "</td></tr>";
  html += String("<tr><td>自動 MQTT モード</td><td class='") +
          (config_.autoPrinterMode ? "ok'>ON" : "warn'>OFF") + "</td></tr>";
  html += String("<tr><td>実況の声</td><td class='") +
          (config_.commentaryVoice ? "ok'>ON" : "warn'>OFF") + "</td></tr>";
  html += F("</table></section>");

  // --- TTS サーバ設定 ---
  html += F("<section class='card'><h3>🔊 TTS サーバー</h3><table>");
  html += "<tr><td>エンジン</td><td>" +
          htmlEscape(config_.ttsEngineType) + "</td></tr>";
  html += "<tr><td>ホスト</td><td>" + htmlEscape(config_.ttsHost) + "</td></tr>";
  html += "<tr><td>ポート</td><td>" + String(config_.ttsPort) + "</td></tr>";
  html += "<tr><td>話者 ID</td><td>" +
          htmlEscape(config_.ttsSpeaker) + "</td></tr>";
  html += String("<tr><td>内蔵ボイス</td><td class='") +
          (runtimeStatus_.voiceClips > 0
               ? "ok'>" + String(runtimeStatus_.voiceClips) + " クリップ" +
                     (runtimeStatus_.voiceFreeText ? "（どんな文章も読める）" : "（実況のみ）")
               : String("warn'>未書き込み")) +
          "</td></tr>";
  html += F("</table></section>");

  // --- アプリ状態 ---
  html += F("<section class='card'><h3>🤖 アプリ</h3><table>");
  html += "<tr><td>モード</td><td>" +
          htmlEscape(runtimeStatus_.appMode) + "</td></tr>";
  html += String("<tr><td>サーボ校正</td><td class='") +
          (runtimeStatus_.servoCalibrated ? "ok'>OK" : "warn'>未校正") +
          "</td></tr>";
  html += String("<tr><td>IMU 校正</td><td class='") +
          (runtimeStatus_.imuCalibrated ? "ok'>OK" : "warn'>未校正") +
          "</td></tr>";
  html += String("<tr><td>カメラ目線</td><td class='") +
          (runtimeStatus_.cameraActive ? "ok'>動作中" : "warn'>OFF") +
          "</td></tr>";
  html += String("<tr><td>Gaming RGB</td><td class='") +
          (config_.gamingRgb ? "ok'>ON" : "warn'>OFF") +
          "</td></tr>";
  html += F("</table></section>");

  // --- 診断（クラッシュ調査用）---
  // Last Reset が PANIC=コードのクラッシュ、BROWNOUT=電源不足、
  // TASK/INT WDT=ハング、POWERON=正常な電源投入。
  // Max Alloc が Free に比べて極端に小さい場合はヒープ断片化のサイン。
  html += F("<section class='card'><h3>🩺 診断</h3><table>");
  {
    const char* rrClass = "ok";
    if (runtimeStatus_.resetReason == "PANIC" ||
        runtimeStatus_.resetReason == "BROWNOUT" ||
        runtimeStatus_.resetReason.indexOf("WDT") >= 0) {
      rrClass = "err";
    }
    html += String("<tr><td>前回のリセット理由</td><td class='") + rrClass + "'>" +
            htmlEscape(runtimeStatus_.resetReason) + "</td></tr>";
  }
  html += "<tr><td>空きヒープ</td><td>" +
          String(runtimeStatus_.freeHeap / 1024) + " KB</td></tr>";
  html += String("<tr><td>最小空きヒープ</td><td class='") +
          (runtimeStatus_.minFreeHeap < 20000 ? "err" : "ok") + "'>" +
          String(runtimeStatus_.minFreeHeap / 1024) + " KB</td></tr>";
  html += String("<tr><td>最大確保ブロック</td><td class='") +
          (runtimeStatus_.maxAllocHeap < 12000 ? "err" : "ok") + "'>" +
          String(runtimeStatus_.maxAllocHeap / 1024) + " KB</td></tr>";
  html += "<tr><td>空き PSRAM</td><td>" +
          String(runtimeStatus_.freePsram / 1024) + " KB</td></tr>";
  html += F("</table></section>");

  // --- システム情報 ---
  const uint32_t uptimeSec = millis() / 1000;
  char uptime[16];
  snprintf(uptime, sizeof(uptime), "%02lu:%02lu:%02lu",
           (unsigned long)(uptimeSec / 3600),
           (unsigned long)((uptimeSec % 3600) / 60),
           (unsigned long)(uptimeSec % 60));

  html += F("<section class='card'><h3>⚙ システム</h3><table>");
  html += "<tr><td>バージョン</td><td>" + String(kAppVersion) + "</td></tr>";
  html += "<tr><td>稼働時間</td><td>" + String(uptime) + "</td></tr>";
  html += "<tr><td>CPU</td><td>" +
          String(ESP.getCpuFreqMHz()) + " MHz</td></tr>";
  html += "<tr><td>Flash</td><td>" +
          String(ESP.getFlashChipSize() / 1024 / 1024) + " MB</td></tr>";
  html += F("</table></section>");

  html += F("<p class='hint'>5 秒ごとに自動更新します</p></main></body></html>");
  return html;
}

// 周辺のWiFiをスキャンして<option>タグのリストを生成する。
// 重複SSIDを除外し、電波強度・セキュリティ情報を付記する。
// 保存済みSSIDが見つかれば selected 属性を付けて事前選択する。
String ConfigPortal::wifiOptionsHtml() {
  String options;
  options.reserve(2000);

  const int count = WiFi.scanNetworks(false, true);
  if (count <= 0) {
    options += F("<option value=''>Wi-Fi が見つかりません</option>");
    if (!config_.wifiSsid.isEmpty()) {
      options += "<option value='" + htmlEscape(config_.wifiSsid) +
                 "' selected>" + htmlEscape(config_.wifiSsid) +
                 "（保存済み）</option>";
    }
    WiFi.scanDelete();
    return options;
  }

  bool currentSsidFound = false;
  for (int i = 0; i < count; ++i) {
    const String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) {
      continue;
    }

    // 重複SSIDは除外する
    bool duplicate = false;
    for (int previous = 0; previous < i; ++previous) {
      if (WiFi.SSID(previous) == ssid) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) continue;

    const int32_t rssi = WiFi.RSSI(i);
    const char* strength =
        rssi >= -55 ? "強" : (rssi >= -70 ? "中" : "弱");
    const bool secured = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    const bool selected = ssid == config_.wifiSsid;
    currentSsidFound |= selected;

    options += "<option value='" + htmlEscape(ssid) + "'";
    if (selected) options += F(" selected");
    options += ">" + htmlEscape(ssid) + "（" + String(strength) +
               " " + String(rssi) + " dBm";
    if (secured) options += F(" 🔒");
    options += F("）</option>");
  }

  // 保存済みSSIDがスキャン結果にない場合（圏外など）は末尾に追加する
  if (!config_.wifiSsid.isEmpty() && !currentSsidFound) {
    options += "<option value='" + htmlEscape(config_.wifiSsid) +
               "' selected>" + htmlEscape(config_.wifiSsid) +
               "（保存済み・圏外）</option>";
  }

  WiFi.scanDelete();
  return options;
}

// GET /api/status が返すJSONを生成する。
// 項目: ok, connected, speaking, ip, tts_host, tts_port, tts_engine
String ConfigPortal::apiStatusJson() {
  const bool spk = speakingProbe_ ? speakingProbe_() : false;
  String ip;
  if (isConnected()) {
    ip = WiFi.localIP().toString();
  } else if (portalActive_ || settingsApActive_) {
    ip = WiFi.softAPIP().toString();
  }

  String j;
  j.reserve(640);
  j += F("{\"ok\":true,\"connected\":");
  j += isConnected() ? F("true") : F("false");
  j += F(",\"speaking\":");
  j += spk ? F("true") : F("false");
  j += F(",\"ip\":\"");
  j += ip;
  j += F("\",\"tts_host\":\"");
  j += jsonEscape(config_.ttsHost);
  j += F("\",\"tts_port\":");
  j += String(config_.ttsPort);
  j += F(",\"tts_engine\":\"");
  j += jsonEscape(config_.ttsEngineType);
  j += F("\",\"printer_monitor\":");
  j += config_.bambuEnabled ? F("true") : F("false");
  j += F(",\"mode\":\"");
  j += jsonEscape(runtimeStatus_.appMode);
  j += F("\",\"reset_reason\":\"");
  j += jsonEscape(runtimeStatus_.resetReason);
  j += F("\",\"free_heap\":");
  j += String(runtimeStatus_.freeHeap);
  j += F(",\"min_free_heap\":");
  j += String(runtimeStatus_.minFreeHeap);
  j += F(",\"max_alloc_heap\":");
  j += String(runtimeStatus_.maxAllocHeap);
  j += F(",\"free_psram\":");
  j += String(runtimeStatus_.freePsram);
  j += F(",\"ui_frames\":");
  j += String(runtimeStatus_.uiFrames);
  j += F(",\"ui_pixels\":");
  j += String(runtimeStatus_.uiPixels);
  j += F(",\"uptime_ms\":");
  j += String(millis());
  j += F("}");
  return j;
}
