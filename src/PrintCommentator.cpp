#include "PrintCommentator.h"

#include <esp_system.h>
#include <math.h>
#include <time.h>

namespace {

// 古くなった Low/Normal コメントは読まずに捨てる（状況が変わっているため）
constexpr uint32_t kStaleCommentMs = 90000;
// 接続断を実況するまでの猶予（瞬断では喋らない）
constexpr uint32_t kOfflineAnnounceMs = 30000;
// 起動後この時間を過ぎても一度もつながらなければ設定の見直しを促す
constexpr uint32_t kFirstConnectGraceMs = 45000;
// 一時停止の実況を遅らせて、理由（stg_cur）が届くのを待つ
constexpr uint32_t kPauseSettleMs = 1500;
// フィラメント切替の実況の最小間隔（多色印刷で連発しないように）
constexpr uint32_t kFilamentCommentIntervalMs = 10UL * 60UL * 1000UL;
// 同じ HMS の組み合わせを繰り返し実況しない期間
constexpr uint32_t kHmsRepeatMs = 10UL * 60UL * 1000UL;

template <size_t N>
const char* pick(const char* const (&options)[N]) {
  return options[esp_random() % N];
}

String temp(float value) {
  if (isnan(value)) return String("-");
  return String(static_cast<int>(lroundf(value)));
}

String quoted(const char* jobName) {
  const String name = speakableJobName(jobName);
  if (name.isEmpty()) return String();
  return "「" + name + "」";
}

// 「〇〇を」「〇〇の」のようにジョブ名に助詞を付ける（名前が無ければ空文字）
String jobWith(const char* jobName, const char* particle) {
  const String q = quoted(jobName);
  if (q.isEmpty()) return String();
  return q + particle;
}

// MQTT 接続失敗の理由を、利用者が次にやることの形で返す
const char* connectHintJa(int mqttState) {
  switch (mqttState) {
    case 4:  // MQTT_CONNECT_BAD_CREDENTIALS
    case 5:  // MQTT_CONNECT_UNAUTHORIZED
      return "アクセスコードが違うみたい。プリンターの画面で確認してね";
    case -2:  // MQTT_CONNECT_FAILED（TCP/TLS に失敗）
    case -4:  // MQTT_CONNECTION_TIMEOUT
      return "プリンターにつながらないよ。IPアドレスと電源を確認してね";
    default:
      return "プリンターにつながらないみたい。設定を確認してね";
  }
}

// 一時停止の理由になる stg_cur（「〜で止まった」の〜部分）
const char* pauseReasonJa(int stage) {
  switch (stage) {
    case 5:  return "M400命令";
    case 6:  return "フィラメント切れ";
    case 17: return "フロントカバー外れ";
    case 20: return "ノズル温度の異常";
    case 21: return "ベッド温度の異常";
    case 23: return "脱調";
    case 26: return "AMSの通信断";
    case 27: return "ヒートブレイクファンの異常";
    case 28: return "チャンバー温度の異常";
    case 30: return "Gコードの一時停止命令";
    case 32: return "ノズルへのフィラメント巻き付き";
    case 33: return "カッターの異常";
    case 34: return "1層目の異常";
    case 35: return "ノズル詰まり";
    default: return nullptr;
  }
}

// 準備中・印刷中に実況する工程のセリフ（nullptr = 実況しない）
const char* stageLineJa(int stage) {
  switch (stage) {
    case 1: {
      static const char* const k[] = {"ベッドのレベリング中。ていねいに測ってるよ",
                                      "オートレベリング中だよ"};
      return pick(k);
    }
    case 2:  return "ベッドを温めてるよ";
    case 3:  return "振動補正の測定中。ちょっと揺れるよ";
    case 4:  return "フィラメントを交換中だよ";
    case 7:  return "ノズルを温めてるよ";
    case 8:  return "押し出しのキャリブレーション中";
    case 9:  return "ベッドの表面をスキャンしてるよ";
    case 10: return "1層目をチェック中だよ";
    case 13: return "ヘッドを原点に戻してるよ";
    case 14: {
      static const char* const k[] = {"ノズルの先をお掃除中",
                                      "ノズルをきれいにしてるよ"};
      return pick(k);
    }
    case 15: return "押出機の温度を確認中";
    case 19: return "流量のキャリブレーション中";
    case 22: return "フィラメントを引き抜いてるよ";
    case 24: return "フィラメントを送り込んでるよ";
    case 25: return "モーターのノイズを調整中";
    case 29: return "チャンバーを冷やしてるよ";
    default: return nullptr;
  }
}

String trayDescription(const PrinterState& s, int trayNow) {
  if (trayNow == 254) return String("外部スプールのフィラメント");
  if (trayNow < 0) return String();
  const int ams = trayNow / PrinterState::kTraysPerAms;
  const int slot = trayNow % PrinterState::kTraysPerAms;
  if (ams >= PrinterState::kMaxAms) return String();
  const AmsTray& t = s.trays[ams][slot];
  if (!t.present) return "AMSの" + String(slot + 1) + "番";
  return String(colorNameJa(t.color)) + "の" + t.type;
}

uint32_t hmsSignature(const PrinterState& s) {
  uint32_t sig = 2166136261u;
  for (uint8_t i = 0; i < s.hmsCount; ++i) {
    sig = (sig ^ s.hms[i].attr) * 16777619u;
    sig = (sig ^ s.hms[i].code) * 16777619u;
  }
  return sig;
}

bool hasHms(const PrinterState& s, const HmsEntry& e) {
  for (uint8_t i = 0; i < s.hmsCount; ++i) {
    if (s.hms[i].attr == e.attr && s.hms[i].code == e.code) return true;
  }
  return false;
}

}  // namespace

String speakableJobName(const char* jobName) {
  if (jobName == nullptr) return String();
  String out;
  size_t codepoints = 0;
  for (const char* p = jobName; *p != '\0';) {
    const uint8_t c = static_cast<uint8_t>(*p);
    size_t len = 1;
    if (c >= 0xF0) len = 4;
    else if (c >= 0xE0) len = 3;
    else if (c >= 0xC0) len = 2;
    if (codepoints >= 24) {
      out += "…";
      break;
    }
    if (len == 1 && (c == '_' || c == '-')) {
      out += ' ';
    } else {
      for (size_t i = 0; i < len && p[i] != '\0'; ++i) out += p[i];
    }
    for (size_t i = 0; i < len && *p != '\0'; ++i) ++p;
    ++codepoints;
  }
  out.trim();
  return out;
}

String etaClockJa(int remainingMin) {
  if (remainingMin < 0) return String();
  const time_t now = time(nullptr);
  if (now < 1700000000) return String();  // NTP 未同期
  const time_t eta = now + static_cast<time_t>(remainingMin) * 60;
  struct tm t;
  localtime_r(&eta, &t);
  char buf[24];
  if (t.tm_min == 0) {
    snprintf(buf, sizeof(buf), "%d時", t.tm_hour);
  } else {
    snprintf(buf, sizeof(buf), "%d時%d分", t.tm_hour, t.tm_min);
  }
  return String(buf);
}

void PrintCommentator::configure(const CommentarySettings& settings) {
  settings_ = settings;
}

void PrintCommentator::update(const PrinterState& s, uint32_t now) {
  onLink(s, now);

  if (!initialized_) {
    if (s.synced && s.link == LinkState::Online) {
      onFirstSync(s);
      prev_ = s;
      initialized_ = true;
    }
    return;
  }
  if (!s.synced) {
    return;
  }

  if (s.phase != prev_.phase) {
    onPhaseChange(prev_, s, now);
  }
  if (s.stage != prev_.stage) {
    onStageChange(prev_, s);
  }
  onProgress(prev_, s);
  onTemperatures(prev_, s);
  onFilament(prev_, s);
  onErrors(prev_, s);

  // 一時停止は理由が届くまで少し待ってから実況する
  if (pendingPauseAt_ != 0) {
    if (s.phase != PrintPhase::Pause) {
      pendingPauseAt_ = 0;  // すぐ再開された
    } else if (now - pendingPauseAt_ >= kPauseSettleMs) {
      pendingPauseAt_ = 0;
      const char* reason = pauseReasonJa(s.stage);
      if (s.stage == 16) {
        say(CommentPriority::Normal, CommentMood::Doubt,
            "一時停止したよ。準備ができたら再開してね");
      } else if (reason != nullptr) {
        say(CommentPriority::High, CommentMood::Doubt,
            String("印刷が一時停止したよ。") + reason +
                "で止まったみたい。確認してね");
      } else {
        say(CommentPriority::High, CommentMood::Doubt,
            "印刷が一時停止したよ。プリンターを確認してね");
      }
    }
  }

  onPeriodic(s, now);
  prev_ = s;
}

void PrintCommentator::onFirstSync(const PrinterState& s) {
  everOnline_ = true;
  String text = "プリンターとつながったよ！";
  CommentMood mood = CommentMood::Happy;
  switch (s.phase) {
    case PrintPhase::Running:
      text += "いま" + jobWith(s.jobName, "を") + "印刷中で、" +
              String(max(s.percent, 0)) + "%まで進んでる。";
      if (s.remainingMin >= 0) {
        text += "残りは" + remainingJa(s.remainingMin) + "くらいだよ";
      }
      break;
    case PrintPhase::Prepare:
      text += "いまは印刷の準備中だね";
      break;
    case PrintPhase::Pause:
      text += "いまは一時停止中みたい";
      mood = CommentMood::Doubt;
      break;
    case PrintPhase::Finish:
      text += "前の印刷は完了してるよ";
      break;
    case PrintPhase::Failed:
      text += "前の印刷は止まっちゃったみたい";
      mood = CommentMood::Doubt;
      break;
    default:
      text += "いまは待機中だね";
      break;
  }
  if (s.hmsCount > 0) {
    text += "。お知らせが" + String(s.hmsCount) + "件あるから、画面を見てみてね";
    mood = CommentMood::Doubt;
  }
  say(CommentPriority::Normal, mood, text);

  const int step = settings_.progressStep;
  lastMilestone_ = (step > 0 && s.percent > 0) ? (s.percent / step) * step : 0;
  firstLayerAnnounced_ = s.layer >= 2;
  lastLayerAnnounced_ = s.totalLayers > 0 && s.layer >= s.totalLayers;
  tenMinutesAnnounced_ = s.remainingMin >= 0 && s.remainingMin <= 10;
  bedReached_ = nozzleReached_ = true;  // 途中からなので温度実況はしない
  announcedStages_ = 0;
  jobStartedAt_ = 0;
  lastHmsSignature_ = hmsSignature(s);
  lastHmsAt_ = millis();
}

void PrintCommentator::resetJob() {
  lastMilestone_ = 0;
  firstLayerAnnounced_ = false;
  lastLayerAnnounced_ = false;
  tenMinutesAnnounced_ = false;
  bedReached_ = false;
  nozzleReached_ = false;
  announcedStages_ = 0;
  jobStartedAt_ = 0;
  lastFilamentCommentAt_ = 0;
}

void PrintCommentator::onPhaseChange(const PrinterState& prev,
                                     const PrinterState& s, uint32_t now) {
  switch (s.phase) {
    case PrintPhase::Prepare:
      if (prev.phase != PrintPhase::Pause) {
        resetJob();
        String text = "印刷の準備を始めたよ！";
        const String job = quoted(s.jobName);
        if (!job.isEmpty()) text += job + "を作るみたい";
        say(CommentPriority::Normal, CommentMood::Happy, text);
      }
      break;

    case PrintPhase::Slicing:
      say(CommentPriority::Low, CommentMood::Neutral,
          "スライス中だよ。ちょっと待ってね");
      break;

    case PrintPhase::Running:
      if (prev.phase == PrintPhase::Pause) {
        static const char* const k[] = {"印刷を再開したよ！",
                                        "再開！続きもがんばろう"};
        say(CommentPriority::Normal, CommentMood::Happy, pick(k));
        break;
      }
      if (prev.phase != PrintPhase::Prepare) {
        resetJob();
      }
      jobStartedAt_ = now;
      {
        String text = "印刷スタート！";
        if (s.totalLayers > 0) {
          text += "全部で" + String(s.totalLayers) + "層";
          text += s.remainingMin >= 0 ? "、" : "あるよ";
        }
        if (s.remainingMin >= 0) {
          text += "だいたい" + remainingJa(s.remainingMin) + "かかる予定だよ";
          const String eta = etaClockJa(s.remainingMin);
          if (!eta.isEmpty()) text += "。終わるのは" + eta + "ごろ";
        }
        say(CommentPriority::Normal, CommentMood::Happy, text);
      }
      break;

    case PrintPhase::Pause:
      pendingPauseAt_ = now == 0 ? 1 : now;
      break;

    case PrintPhase::Finish: {
      String text = "印刷完了！";
      const String job = quoted(s.jobName);
      if (!job.isEmpty()) text += job + "ができたよ。";
      if (jobStartedAt_ != 0) {
        const int minutes = static_cast<int>((now - jobStartedAt_) / 60000UL);
        text += "かかった時間は" + remainingJa(minutes) + "。";
      }
      text += "おつかれさま！";
      say(CommentPriority::High, CommentMood::Happy, text, String(), true);
      jobStartedAt_ = 0;
      break;
    }

    case PrintPhase::Failed:
      if (isCancelError(s.printError)) {
        say(CommentPriority::Normal, CommentMood::Sad,
            "印刷がキャンセルされたよ。また今度がんばろう");
      } else {
        say(CommentPriority::High, CommentMood::Sad,
            "あっ、印刷が止まっちゃった…プリンターの画面を確認してね");
      }
      jobStartedAt_ = 0;
      break;

    case PrintPhase::Idle:
      if (prev.phase == PrintPhase::Running ||
          prev.phase == PrintPhase::Prepare ||
          prev.phase == PrintPhase::Pause) {
        say(CommentPriority::Normal, CommentMood::Doubt,
            "印刷が終わったみたい。プリンターは待機中になったよ");
        jobStartedAt_ = 0;
      }
      break;

    default:
      break;
  }
}

void PrintCommentator::onStageChange(const PrinterState& prev,
                                     const PrinterState& s) {
  (void)prev;
  if (s.stage < 0) return;

  // 一時停止中に理由が変わった（一時停止の実況後に届いた場合）
  if (s.phase == PrintPhase::Pause && pendingPauseAt_ == 0) {
    const char* reason = pauseReasonJa(s.stage);
    if (reason != nullptr) {
      say(CommentPriority::High, CommentMood::Doubt,
          String("一時停止の理由は") + reason + "みたい");
    }
    return;
  }

  if (!settings_.stages) return;
  if (s.phase != PrintPhase::Prepare && s.phase != PrintPhase::Running) return;
  if (s.stage < 64) {
    const uint64_t bit = 1ULL << s.stage;
    if (announcedStages_ & bit) return;  // 同じジョブで同じ工程は1回だけ
    const char* line = stageLineJa(s.stage);
    if (line == nullptr) return;
    announcedStages_ |= bit;
    say(CommentPriority::Low, CommentMood::Neutral, line);
  }
}

void PrintCommentator::onProgress(const PrinterState& prev,
                                  const PrinterState& s) {
  if (s.phase != PrintPhase::Running) return;

  // 1層目・最終層
  if (!firstLayerAnnounced_ && prev.layer == 1 && s.layer >= 2) {
    firstLayerAnnounced_ = true;
    static const char* const k[] = {
        "1層目が終わったよ！ここを越えればひと安心",
        "1層目完了！いい感じに乗ってるといいな"};
    say(CommentPriority::Normal, CommentMood::Happy, pick(k));
  }
  if (!lastLayerAnnounced_ && s.totalLayers > 1 &&
      s.layer >= s.totalLayers && prev.layer < s.totalLayers) {
    lastLayerAnnounced_ = true;
    say(CommentPriority::Normal, CommentMood::Happy, "最後の層に入ったよ！");
  }

  // 残り10分
  if (!tenMinutesAnnounced_ && prev.remainingMin > 10 &&
      s.remainingMin >= 0 && s.remainingMin <= 10) {
    tenMinutesAnnounced_ = true;
    say(CommentPriority::Normal, CommentMood::Happy,
        "あと10分くらいで完成だよ！");
  }

  // 進捗マイルストーン
  const int step = settings_.progressStep;
  if (step <= 0 || s.percent <= 0) return;
  const int milestone = (s.percent / step) * step;
  if (milestone <= lastMilestone_ || milestone >= 100) return;
  lastMilestone_ = milestone;

  const String pct = String(milestone) + "%";
  const String rem =
      s.remainingMin >= 0 ? remainingJa(s.remainingMin) : String("不明");
  String text;
  if (milestone == 50) {
    text = "半分まで来たよ！残りは" + rem + "くらい";
  } else if (milestone >= 90) {
    text = pct + "！もうすぐ完成だよ。あと" + rem;
  } else {
    switch (esp_random() % 3) {
      case 0:
        text = pct + "まで進んだよ。残り" + rem + "くらい";
        break;
      case 1:
        text = "いま" + pct + "、" +
               (s.layer > 0 ? String(s.layer) + "層目だよ" : String("順調だよ"));
        break;
      default:
        text = pct + "通過！順調だね";
        break;
    }
  }
  say(CommentPriority::Normal,
      milestone >= 50 ? CommentMood::Happy : CommentMood::Neutral, text);
}

void PrintCommentator::onTemperatures(const PrinterState& prev,
                                      const PrinterState& s) {
  if (!settings_.temps || s.phase != PrintPhase::Prepare) return;

  if (!bedReached_ && !isnan(s.bedTarget) && s.bedTarget >= 40.0f &&
      !isnan(prev.bedTemp) && !isnan(s.bedTemp) &&
      prev.bedTemp < s.bedTarget - 1.5f && s.bedTemp >= s.bedTarget - 1.5f) {
    bedReached_ = true;
    say(CommentPriority::Low, CommentMood::Neutral,
        "ベッドが" + temp(s.bedTarget) + "度になったよ");
  }
  // P1S はレベリング前に 140℃ 付近まで予熱するので、印刷温度帯だけ実況する
  if (!nozzleReached_ && !isnan(s.nozzleTarget) && s.nozzleTarget >= 170.0f &&
      !isnan(prev.nozzleTemp) && !isnan(s.nozzleTemp) &&
      prev.nozzleTemp < s.nozzleTarget - 2.0f &&
      s.nozzleTemp >= s.nozzleTarget - 2.0f) {
    nozzleReached_ = true;
    say(CommentPriority::Low, CommentMood::Happy,
        "ノズルが" + temp(s.nozzleTarget) + "度になったよ。準備オーケー");
  }
}

void PrintCommentator::onFilament(const PrinterState& prev,
                                  const PrinterState& s) {
  if (s.trayNow == prev.trayNow || s.trayNow < 0) return;
  if (s.phase != PrintPhase::Prepare && s.phase != PrintPhase::Running) return;
  const uint32_t now = millis();
  if (lastFilamentCommentAt_ != 0 &&
      now - lastFilamentCommentAt_ < kFilamentCommentIntervalMs) {
    return;
  }
  const String desc = trayDescription(s, s.trayNow);
  if (desc.isEmpty()) return;
  lastFilamentCommentAt_ = now;
  if (prev.trayNow < 0) {
    say(CommentPriority::Low, CommentMood::Neutral, desc + "をセットしたよ");
  } else {
    say(CommentPriority::Low, CommentMood::Neutral, desc + "に切り替えたよ");
  }
}

void PrintCommentator::onErrors(const PrinterState& prev,
                                const PrinterState& s) {
  // HMS（新しいお知らせが増えたときだけ）
  bool hasNew = false;
  for (uint8_t i = 0; i < s.hmsCount; ++i) {
    if (!hasHms(prev, s.hms[i])) {
      hasNew = true;
      break;
    }
  }
  if (hasNew) {
    const uint32_t sig = hmsSignature(s);
    const uint32_t now = millis();
    if (sig != lastHmsSignature_ || now - lastHmsAt_ >= kHmsRepeatMs) {
      lastHmsSignature_ = sig;
      lastHmsAt_ = now;
      const String speech = "プリンターからお知らせが" + String(s.hmsCount) +
                            "件あるよ。画面を確認してね";
      say(CommentPriority::High, CommentMood::Doubt,
          speech + "（" + hmsCodeString(s.hms[0]) + "）", speech);
    }
  } else if (prev.hmsCount > 0 && s.hmsCount == 0) {
    say(CommentPriority::Low, CommentMood::Happy, "お知らせ表示が消えたよ");
  }

  // print_error（キャンセル以外、失敗状態の実況と重ならない場合）
  if (prev.printError == 0 && s.printError != 0 &&
      !isCancelError(s.printError) &&
      (s.phase == PrintPhase::Running || s.phase == PrintPhase::Prepare)) {
    char code[16];
    snprintf(code, sizeof(code), "%08lX", (unsigned long)s.printError);
    const String speech = "エラーが出たみたい。プリンターを見てあげて";
    say(CommentPriority::High, CommentMood::Angry,
        speech + "（" + code + "）", speech);
  }
}

void PrintCommentator::onLink(const PrinterState& s, uint32_t now) {
  if (s.link == LinkState::Online) {
    if (offlineAnnounced_) {
      say(CommentPriority::Normal, CommentMood::Happy,
          "プリンターとまたつながったよ");
    }
    offlineSince_ = 0;
    offlineAnnounced_ = false;
    return;
  }
  if (s.link == LinkState::Disabled) return;
  if (!everOnline_) {
    if (s.link == LinkState::Error && !neverOnlineAnnounced_ &&
        now >= kFirstConnectGraceMs) {
      neverOnlineAnnounced_ = true;
      say(CommentPriority::Normal, CommentMood::Doubt,
          connectHintJa(s.mqttErrorCode));
    }
    return;
  }
  if (offlineSince_ == 0) {
    offlineSince_ = now == 0 ? 1 : now;
    return;
  }
  if (!offlineAnnounced_ && now - offlineSince_ >= kOfflineAnnounceMs) {
    offlineAnnounced_ = true;
    say(CommentPriority::Normal, CommentMood::Sad,
        "プリンターとの接続が切れちゃった…");
  }
}

void PrintCommentator::onPeriodic(const PrinterState& s, uint32_t now) {
  if (settings_.periodicMin == 0 || s.phase != PrintPhase::Running) return;
  if (now - lastCommentAt_ < settings_.periodicMin * 60000UL) return;

  String text;
  switch (esp_random() % 3) {
    case 0:
      text = "いま" + String(max(s.percent, 0)) + "%、";
      if (s.layer > 0 && s.totalLayers > 0) {
        text += String(s.layer) + "層目。全部で" + String(s.totalLayers) + "層だよ";
      } else {
        text += "順調に進んでるよ";
      }
      break;
    case 1:
      text = "ノズル" + temp(s.nozzleTemp) + "度、ベッド" + temp(s.bedTemp) +
             "度で安定してるよ。いま" + String(max(s.percent, 0)) + "%";
      break;
    default:
      text = "現在" + String(max(s.percent, 0)) + "%。";
      if (s.remainingMin >= 0) {
        text += "残り" + remainingJa(s.remainingMin) + "くらいだよ";
        const String eta = etaClockJa(s.remainingMin);
        if (!eta.isEmpty()) text += "。" + eta + "ごろ完成予定";
      }
      break;
  }
  say(CommentPriority::Low, CommentMood::Neutral, text);
  // Low は詰まっていると捨てられるが、次回また試すので lastCommentAt_ は push 側で更新
}

Comment PrintCommentator::statusReport(const PrinterState& s) const {
  Comment c;
  c.priority = CommentPriority::Normal;
  c.createdAt = millis();

  if (s.link == LinkState::Disabled) {
    c.text = "プリンターの監視はオフになってるよ。設定画面でつないでね";
    c.mood = CommentMood::Doubt;
    return c;
  }
  if (s.link != LinkState::Online || !s.synced) {
    c.text = "プリンターとつながってないみたい。電源とネットワークを確認してね";
    c.mood = CommentMood::Sad;
    return c;
  }

  switch (s.phase) {
    case PrintPhase::Running: {
      c.mood = CommentMood::Happy;
      c.text = jobWith(s.jobName, "を") + "印刷中。いま" +
               String(max(s.percent, 0)) + "%";
      if (s.layer > 0 && s.totalLayers > 0) {
        c.text += "、" + String(s.layer) + "層目、全部で" +
                  String(s.totalLayers) + "層";
      }
      if (s.remainingMin >= 0) {
        c.text += "。残りは" + remainingJa(s.remainingMin) + "くらい";
        const String eta = etaClockJa(s.remainingMin);
        if (!eta.isEmpty()) c.text += "で、" + eta + "ごろ完成予定";
      }
      c.text += "だよ";
      break;
    }
    case PrintPhase::Prepare: {
      c.text = "印刷の準備中だよ。";
      const char* stage = stageLabelJa(s.stage);
      if (stage != nullptr && s.stage != 0) {
        c.text += String("いまは") + stage + "。";
      }
      c.text += "ノズル" + temp(s.nozzleTemp) + "度、ベッド" + temp(s.bedTemp) +
                "度";
      break;
    }
    case PrintPhase::Pause: {
      c.mood = CommentMood::Doubt;
      c.text = "一時停止中だよ。";
      const char* reason = pauseReasonJa(s.stage);
      if (reason != nullptr) c.text += String(reason) + "で止まってるみたい。";
      c.text += String(max(s.percent, 0)) + "%まで進んでるよ";
      break;
    }
    case PrintPhase::Finish:
      c.mood = CommentMood::Happy;
      c.text = jobWith(s.jobName, "の") + "印刷は完了してるよ。取り出してあげてね";
      break;
    case PrintPhase::Failed:
      c.mood = CommentMood::Sad;
      c.text = isCancelError(s.printError)
                   ? "前の印刷はキャンセルされたよ"
                   : "前の印刷は止まっちゃったみたい";
      break;
    case PrintPhase::Slicing:
      c.text = "スライス中だよ";
      break;
    default:
      c.mood = CommentMood::Sleepy;
      c.text = "プリンターは待機中だよ。ノズル" + temp(s.nozzleTemp) +
               "度、ベッド" + temp(s.bedTemp) + "度";
      break;
  }

  if (s.hmsCount > 0) {
    c.text += "。お知らせが" + String(s.hmsCount) + "件あるよ";
  }
  return c;
}

CommentPriority PrintCommentator::peekPriority() const {
  if (count_ == 0) return CommentPriority::Low;
  return queue_[head_].priority;
}

Comment PrintCommentator::popComment() {
  while (count_ > 0) {
    Comment c = queue_[head_];
    queue_[head_] = Comment();
    head_ = (head_ + 1) % kQueueSize;
    count_--;
    if (c.priority != CommentPriority::High &&
        millis() - c.createdAt > kStaleCommentMs) {
      continue;  // 古い Low/Normal は捨てる
    }
    return c;
  }
  return Comment();
}

void PrintCommentator::remember(const Comment& comment) {
  if (comment.text.isEmpty()) return;
  historyHead_ = (historyHead_ + kHistorySize - 1) % kHistorySize;
  history_[historyHead_] = comment;
  if (historyCount_ < kHistorySize) historyCount_++;
}

const Comment& PrintCommentator::history(uint8_t index) const {
  return history_[(historyHead_ + index) % kHistorySize];
}

void PrintCommentator::say(CommentPriority priority, CommentMood mood,
                           const String& text, const String& speech,
                           bool celebrate) {
  Comment c;
  c.text = text;
  c.speech = speech;
  c.mood = mood;
  c.priority = priority;
  c.celebrate = celebrate;
  push(c);
}

void PrintCommentator::push(Comment c) {
  c.createdAt = millis();

  // Low は他に待ちがあれば捨てる（連続実況で渋滞しないように）
  if (c.priority == CommentPriority::Low && count_ > 0) {
    return;
  }

  // High は待ちの Low/Normal を押しのける
  if (c.priority == CommentPriority::High && count_ > 0) {
    Comment kept[kQueueSize];
    uint8_t keptCount = 0;
    for (uint8_t i = 0; i < count_; ++i) {
      const Comment& q = queue_[(head_ + i) % kQueueSize];
      if (q.priority == CommentPriority::High) kept[keptCount++] = q;
    }
    for (uint8_t i = 0; i < kQueueSize; ++i) queue_[i] = Comment();
    head_ = 0;
    count_ = keptCount;
    for (uint8_t i = 0; i < keptCount; ++i) queue_[i] = kept[i];
  }

  if (count_ == kQueueSize) {
    // 満杯なら最も古いものを捨てる
    queue_[head_] = Comment();
    head_ = (head_ + 1) % kQueueSize;
    count_--;
  }
  queue_[(head_ + count_) % kQueueSize] = c;
  count_++;
  lastCommentAt_ = c.createdAt;
}
