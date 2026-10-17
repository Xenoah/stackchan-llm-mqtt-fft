# StackChan × Bambu Lab 実況モニター（stackchan-mqtt）

StackChan（CoreS3）が Bambu Lab P1S を LAN MQTT で監視し、印刷状況を声・表情・字幕・LED で
実況するプロジェクト。stackchan-codex v1.1.5 をベースに、ESP32-bambu-MQTT の
Bambu LAN MQTT 実装を StackChan 向けに再構成した。プリンター関連は **Part E** を参照。

以下 Part A〜D はベース（stackchan-codex）の仕様。StackChan は「薄い再生クライアント」で、
Android Termux 上の Gateway が LLM 推論・音声生成・履歴管理を担う（STT は使わない）。

- ベース: stackchan-codex v1.1.5（release tag β3.5.0 の simple_wav 方式を発展）
- main ブランチの ConversationController/STT/LLM 分離実装は**使わない**

---

## 全体フロー

1. Android Termux で Gateway を起動（llama-server + FastAPI）
2. スマホ/PC ブラウザから Gateway UI を開く
3. パスワードでログイン
4. 質問入力 + 回答言語/長さ/TTS音声/ピッチ/音量/速度 を設定
5. 「送信して喋らせる」
6. Gateway が LLM で回答生成 → 同じテキストから WAV 生成
7. Gateway が StackChan の `POST /api/speak` を呼ぶ
8. StackChan が Gateway の `POST /synthesis` から WAV を取得し発話
9. （A ボタン）StackChan が `__REASK_LAST__` を /synthesis に POST → 最後の質問を再 LLM 処理して発話

### 重要仕様
- 表示回答と発話内容は意味として一致させる
- デフォルト日本語。英語選択時は表示も発話も英語
- 日本語は表示が漢字混じり、音声生成時のみ漢字→かな変換可。**読み上げ用テキストも UI に表示**
- 設定は確実に永続保存。履歴から再発話/声だけ変更/削除が可能
- パスワードログインでセキュリティ確保

---

## Part A: StackChan ファームウェア（最小変更）

β3.5.0 をベースに以下のみ追加・変更する。

| # | 変更 | 状態 |
|---|------|------|
| A1 | `M5.Speaker.setVolume(160)` → `255` | ✅ |
| A2 | `speakConfiguredText()` を `speakText(text)` にリファクタ（A ボタンは従来通り設定テキストを発話） | ✅ |
| A3 | `POST /api/speak` 追加。busy なら `{"ok":false,"error":"busy"}`、受付なら `{"ok":true}` | ✅ |
| A4 | `GET /api/status` 追加。`ok/connected/speaking/ip/tts_host/tts_port/tts_engine` を返す | ✅ |
| A5 | A ボタン運用: 設定の「Text to speak」に `__REASK_LAST__` を入れる（コード変更不要） | ✅ |

### 設計メモ
- `/api/speak` は WebServer ハンドラ内で同期再生せず、`pendingApiSpeak` フラグを立てて即 `{"ok":true}` を返す。実際の TTS 再生は `loop()` が拾って実行（再生はブロッキングのため）。
- `/api/speak` のデフォルト発話テキストは `__CURRENT__`（Gateway は current.wav を返す＝再 LLM しない）。`text` フォーム引数で上書き可。
- A ボタン → `speakConfiguredText()` → 設定テキスト（`__REASK_LAST__`）を /synthesis に POST → Gateway が再 LLM。
- `speaking` 状態は `ConfigPortal` にプローブ用コールバックで渡す。

### StackChan 側 想定設定
- TTS Engine Type: `simple_wav`
- TTS Host: Android スマホの LAN 内 IP
- TTS Port: `50021`
- Speaker / Style ID: `3`
- Text to speak: `__REASK_LAST__`

---

## Part B: Android Termux Gateway（FastAPI）

`gateway/` 配下に実装。

- llama.cpp `llama-server` → `http://127.0.0.1:8080/v1/chat/completions`
- Gateway listen → `0.0.0.0:50021`
- TTS: `espeak-ng`（ja は pykakasi で漢字→かな変換）
- 保存先:
  - `~/stackchan_gateway/settings.json`
  - `~/stackchan_gateway/history/`
  - `~/stackchan_gateway/current.json` / `current.wav`
  - `~/stackchan_gateway/logs/`

### settings.json 項目
`password_hash, password_salt, session_secret, stackchan_host, stackchan_port,
voice_lang, tts_voice, pitch, volume, speed, answer_mode, kanji_to_kana, auto_speak,
max_history, system_prompt`

デフォルト: stackchan_port=80, voice_lang=ja, tts_voice=default, pitch=80, volume=200, speed=150,
answer_mode=short, kanji_to_kana=true, auto_speak=true, max_history=50。
初期パスワード `stackchan`（salt+sha256 で保存、平文不可）。

### エンドポイント
`GET /`, `POST /login`, `POST /logout`, `GET /status`, `POST /settings`,
`POST /ask`, `POST /voice/rebuild`, `GET /current.wav`,
`POST /synthesis`（StackChan 互換: `__REASK_LAST__` で再 LLM、通常は current.wav）,
`POST /stackchan/speak`, `GET /history`,
`POST /history/{id}/speak`, `POST /history/{id}/revoice`, `POST /history/{id}/delete`

### 音声生成
- ja: `espeak-ng -v ja`（必要に応じ pykakasi で読み変換、読み上げ用テキストを保存・表示）
- en: `espeak-ng -v en-us`
- tts_voice: `default`, `f1`, `f2`, `m1`, `m2`, `m3`（espeak-ng の `+f1` 等を付与）
- pitch `-p` 0–99（既定80）, volume `-a` 0–200（既定200）, speed `-s` 80–260（既定150）

### 完了条件
1. β3.5.0 ベースでビルド可 ✅
2. スピーカー音量 255 ✅
3. `/api/speak` 追加 ✅
4. ブラウザ送信で A ボタン不要で発話 ✅（/ask → set_current → /api/speak → /synthesis）
5. A ボタンで最後の質問を再 LLM 発話 ✅（`__REASK_LAST__` → process_question）
6. TTS音声/ピッチ/音量/速度をブラウザ変更可 ✅（設定保存 + revoice）
7. 音声だけ再生成可 ✅（/voice/rebuild, /history/{id}/revoice）
8. settings.json 永続保存 ✅（原子的書き込み）
9. 履歴から再発話/声変更/削除 ✅
10. パスワードログイン ✅（salt+sha256, 署名Cookie）
11. 表示回答と発話一致 ✅（answer=表示, speech_text=読み上げ。両方UI表示）
12. デフォルト日本語 ✅

### 既知の制約
- `/synthesis` の `__REASK_LAST__` は LLM 推論を**同期実行**する。StackChan(simple_wav)
  の HTTP タイムアウトは約 30 秒（`VoiceVoxClient.cpp` `kNetworkTimeoutMs`）なので、
  モデルが遅いと A ボタン再発話がタイムアウトする可能性がある。短い回答長(`hitokoto`/`short`)
  と小型モデル推奨。ブラウザ送信フローは LLM を先に終えてから `/api/speak` を呼ぶため影響なし。

---

## Part C: 安定化・表現強化・カメラ目線（2026-06-28）

| # | 項目 | 内容 | 状態 |
|---|------|------|------|
| C1 | クラッシュ原因修正 | アバター描画の `init(8)`→`init(1)`。m5avatar は毎フレーム 320x240 スプライトを createSprite/deleteSprite する。colorDepth=8 だと約77KB/フレームの連続確保が必要で、WiFi/TTS/HTML 確保とぶつかりヒープ断片化→確保失敗→クラッシュ（全状況・放置中も発生）。colorDepth=1 で約9.6KB/フレームに削減し断片化耐性を8倍に。見た目は StackChan 本来の2トーン（パレットの前景/背景色は反映） | ✅ |
| C2 | クラッシュ診断 | 起動時の再起動理由（PANIC/BROWNOUT/TASK_WDT 等）と空きヒープ/最小ヒープ/最大連続ブロック/PSRAM を `/status` ページ・`/api/status`・シリアル(15秒毎)に表示。次回クラッシュ後に `/status` の **Last Reset** を見れば原因種別が分かる | ✅ |
| C3 | 漢字読み上げ | Gateway で読み上げ前にマークダウン/装飾記号を除去（`sanitize_for_speech`）してから pykakasi で かな 変換。espeak が「アスタリスク」等と読むのを防止 | ✅ |
| C4 | ピッチ上げ | Gateway デフォルト pitch 80→90（UIで 0–99 調整可、最大99） | ✅ |
| C5 | 顔の動きを大げさに | 口パク開き具合を1.5倍に増幅 + Normal変形時に呼吸ズーム（±6%, 約2.4秒周期）で顔全体がゆっくり拡大縮小 | ✅ |
| C6 | カメラ目線 | CoreS3 内蔵カメラ(GC0308)を QQVGA RGB565 で取得し、輝度重心の方向へ視線(`setGaze`)。4fps・発話中/メニュー中は休止・初期化失敗時は自動無効。設定ページの **Camera gaze** で ON/OFF（NVS保存） | ✅ |

### カメラ目線の注意（重要）
- CoreS3 ではカメラの SCCB が**内部I2C（タッチ/IMU/PMICと同じ 11/12 ピン）を共有**する。
  `esp_camera_init` 前に `M5.In_I2C.release()` が必要なため、**カメラ有効時はタッチ操作が
  効かなくなる可能性**がある。その場合はスマホの設定ページで **Camera gaze を OFF→保存**
  すれば、再起動後カメラを初期化せずタッチが復帰する（起動時のサーボ選択ダイアログは
  カメラ初期化より前なので影響しない）。
- 左右が逆に見える場合は `main.cpp` の `kCamGazeHSign` を `-1` にする。
- カメラは PSRAM を使用（QQVGA 2バッファ ≈ 76KB）。本体の動作はクラッシュ修正(C1)後に確認推奨。

---

## Part D: 設定画面チャットUI・ゲーミングRGB（2026-06-28 / v1.1.5）

| # | 項目 | 内容 | 状態 |
|---|------|------|------|
| D1 | Local LLM Chat UI | ConfigPortal の設定ページ（`GET /`）に「Local LLM Chat」カードを追加。textarea（初期値「自己紹介して」）+ ボタン「LLMで回答して喋る」+ status + 回答用 `<pre>`。ブラウザの `fetch` が `http://<tts_host>:<tts_port>/ask` へ `application/x-www-form-urlencoded` の `text` をPOSTし、JSON（`question`/`answer`/`stackchan`）を整形表示。**ESP32側に中継APIは作らない**（メモリ節約のためJSON処理はブラウザ側）。送信中 `thinking...`、エラー時 `fetch error: 詳細`。Gateway URL は保存済み `config_.ttsHost`/`ttsPort` をJSへ埋め込み。Gateway は `Access-Control-Allow-Origin: *` 前提 | ✅ |
| D2 | ゲーミングRGB（画面） | `AvatarFaceController` に虹色循環を追加。`update()` 末尾で `updateGamingPalette()`（約6秒で1周・約30fps throttle）。colorDepth=1 の2トーン描画に合わせ、背景=虹色／前景=補色で循環。`setGamingRgb(bool)`/`isGamingRgb()`/`gamingHue()` を公開。OFFで通常パレットへ即復帰 | ✅ |
| D3 | ゲーミングRGB（LED） | `main.cpp` の `serviceApp()` から `updateGamingLed()` を毎フレーム実行。色相は `avatarFace.gamingHue()` と同期、明度控えめ。ステータスLEDは `showStatusLed(r,g,b,holdMs)` 経由に置換し、指定時間だけ色を保持してから虹色へ戻す（TTS=青/エラー=赤(3秒)/通常=緑 等の意味を維持） | ✅ |
| D4 | 設定永続化・UI | `AppConfig.gamingRgb`（既定true）を追加。NVSキー `gaming_rgb` で load/save。設定ページにチェックボックス「Gaming RGB (rainbow face & LED)」、`/status` の App カードに On/Off 表示。起動時 `configPortal.begin()` 後に `avatarFace.setGamingRgb(config.gamingRgb)` で反映 | ✅ |

---

## Part E: Bambu Lab P1S 監視と実況（2026-09-24 / v2.0.0）

| # | 項目 | 内容 | 状態 |
|---|------|------|------|
| E1 | MQTT クライアント | `BambuMqttClient`: `mqtts://<ip>:8883`、`bblp` / アクセスコード、`setInsecure()`。専用 FreeRTOS タスク（コア0、12KB スタック）で接続・`loop()`。失敗しても停止せず 3〜60 秒の指数バックオフで再接続。接続時と10分ごとに pushall（手動は20秒間隔に制限）。送信は retain=false。コマンドは pushall とチャンバーライト（`system.ledctrl`）のみ | ✅ |
| E2 | 状態モデル | `PrinterState`: P1 系の差分 push をフィールド単位でマージ。`gcode_state`/`mc_percent`/`mc_remaining_time`/`layer_num`/`total_layer_num`/`stg_cur`/`print_error`/`spd_lvl`/温度/ファン/AMS/HMS/ライト。ArduinoJson フィルタで必要項目だけ解析、受信バッファ 24KB（PSRAM）。共有はミューテックス＋コピー（`snapshot()`）と `revision()` | ✅ |
| E3 | 実況エンジン | `PrintCommentator`: 前回スナップショットとの差分→優先度付きキュー（High は Low/Normal を押しのける、Low は待ちがあれば捨てる、古い Normal は90秒で破棄）。開始/工程/温度/進捗/1層目/最終層/残り10分/AMS切替/一時停止（理由待ち1.5秒）/再開/完了（所要時間・喜びモーション）/失敗/キャンセル（`0x0300400C`）/HMS/print_error/接続断(30秒)/未接続(45秒・原因別ヒント)/定期報告/状況報告 | ✅ |
| E4 | 顔 HUD | `FaceHud` + `HudMouth`: 全顔テンプレートの口 Drawable を包み、m5avatar の 1bit 顔スプライト内に HUD を描く（描画タスク内で完結し LCD 取り合いなし）。上段=状態チップ(警告時点滅)/進捗%/残り/進捗バー、下段=層・温度・ジョブ名・完成予定、実況中は2行字幕（3行以上は3.2秒ページ送り、約物ぶら下げ）。`showStatus` は HUD 表示中トースト表示に切替。HUD 表示中は呼吸ズームを停止 | ✅ |
| E5 | 本体 UI | 顔タップ→フルカラーのプリンター詳細画面（`PrinterScreen`）。メニューを2×3タイル化（プリンター/実況の声/LOCAL LLM/LEVEL HOLD/設定/閉じる）。LED 進捗リング（左右6灯、端数灯が脈動、準備=青呼吸、一時停止=黄点滅、完了=緑5分、失敗=赤点滅5分）。起動時サーボ確認は10秒無操作で NO | ✅ |
| E6 | Web UI | `/` をダッシュボード化（PROGMEM 静的 HTML が `/api/printer` を2秒ごとにポーリング）。`/settings`・`/status` を日本語・共通 `/app.css` で再デザイン。API: `/api/printer`、`/report`、`/refresh`、`/light`、`/voice`、`/say` | ✅ |
| E7 | 設定 | NVS: `bb_on`/`bb_host`/`bb_serial`/`bb_code`（コードは再表示しない）、`cm_voice`/`cm_step`/`cm_period`/`cm_stages`/`cm_temps`、`hud`/`led_prog`/`tz`（既定 `JST-9`、NTP は ntp.nict.jp ほか）、`quiet_on`/`quiet_from`/`quiet_to`（夜間は High だけ声に出す。日付またぎ対応・時刻未同期なら無効）、`volume`（M5.Speaker 0〜255） | ✅ |
| E8 | Gateway | `/synthesis` 本文 `__SAY__<文章>` を LLM なしで読み上げ（`say.wav`、ロック付き）。simple_wav 時はファームが実況文に接頭辞を付ける | ✅ |

### 設計メモ
- HTTP ハンドラは TTS 再生中（`serviceApp()` のネスト）にも走るため、Web からの「話して」系は
  `commentator.enqueue()` に積むだけにし、実際の発話は `loop()` の `performComment()` で行う。
- `performComment()` は `speaking=true` の間 `playTts()` をブロッキング実行。その間も
  `serviceApp()` → `updatePrinterMonitor()` で状態取り込みと実況判定は続く。
- 16bit のフルスクリーン canvas（メニュー/詳細画面）は `setPsram(true)` で PSRAM に置く。
  mbedTLS は内部 RAM（`CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC`）で約40KB使うため。
- `stg_cur` の意味は OpenBambuAPI / ha-bambulab のコミュニティ調査に基づく（`stageLabelJa()`）。

### 既知の制約
- ホスト環境に C++ コンパイラが無く、実況ロジックの単体テストは未整備（実機ログ `[commentary]` で確認）。
- 実機（P1S）での通し確認は未実施。確認観点: 接続（state=5 はアクセスコード誤り）、pushall サイズ、
  HUD の字幕の折り返し、LED の左右の並び順、完了時の喜びモーション。
- 設定変更は保存→再起動で反映（実況の声 ON/OFF のみ即時反映・NVS 保存）。

---

## Part F: 内蔵ボイス（TTS サーバーなしで喋る）（2026-09-24）

| # | 項目 | 内容 | 状態 |
|---|------|------|------|
| F1 | ボイスパック生成 | `tools/make_voice_pack.py`: `src/PrintCommentator.cpp`・`src/PrinterState.cpp` の日本語文字列リテラル（書式指定を含むものは除く）＋材質名・あいさつ＋数字キー `#N`（0〜99・百の位・千の位）＋促音/連濁つき `=N単位`（%・分・時間・時・層・件）を VOICEVOX で合成。先頭の句読点は読み上げから外す。前後の無音を詰め、G.711 μ-law 12kHz（SNR 約37dB、約8.5MB）で `data/voice.pak`。IMA-ADPCM 16kHz も `--codec adpcm` で選べるが声では SNR 15dB 前後でザラつく。`.pio/voice_cache` に合成結果をキャッシュ | ✅ |
| F2 | パーティション | `partitions.csv`: nvs/otadata は既定と同じ位置、app0 3MB（OTA なし）、LittleFS（ラベル spiffs）12.8MB。**このファイルは ASCII のみ**（PlatformIO が cp1252 で読む） | ✅ |
| F3 | 再生 | `BuiltinVoice`: 索引を PSRAM に読み、先頭バイトごとのバケットで最長一致。数字は数字で始まる断片を優先→なければ千・百の位＋`=N単位`（単位で始まる長い断片があれば使わない）→`#N`。「」内は「作品」。句読点は間（。220ms、、110ms）。3面バッファで `playRaw`、TtsClient と同じ基準で口パク | ✅ |
| F4 | 切り替え | エンジン `builtin` を追加。`speakSentence()`: サーバー指定でも失敗したら内蔵ボイスで言い直し、3分間はサーバーを休ませる。`speakText()`（A ボタン・/api/speak）も Wi-Fi なし・失敗時は内蔵ボイス（読めなければあいさつ）。TtsClient の接続タイムアウトを 30s→4s。起動時に内蔵ボイスであいさつ（実況の声 OFF なら無し） | ✅ |

### 検証
- パックを Python で読み戻し、μ-law の往復 SNR 約 37dB を確認。
- 分解規則を Python で再現し、代表的な実況文 20 件がすべて読み飛ばしなしで分解できることを確認。
- 実機: `[voice] builtin voice ready: 678 clips, mulaw 12000Hz`。TTS サーバー到達不能時に `/api/speak` →
  4 秒後に内蔵ボイスへ切り替わり再生（`[voice] builtin: 3 steps`）。
- 注意: ESP32-S3 の USB シリアルはポートを開閉すると DTR/RTS でリセットされることがある。
  ログを取るときは DTR/RTS を下げたまま開く。

---

## Part G: MQTT モードへの自動切り替え（2026-09-25 / v2.1.0）

| # | 項目 | 内容 | 状態 |
|---|------|------|------|
| G1 | MQTT モード | `AppMode::Printer`（表示名 `MQTT`）。HUD は MQTT モードの間だけ表示、頭タップは状況報告。LEVEL HOLD 以外は首のアイドル動作・喜びモーションを使う（`modeUsesBodyMotion()`） | ✅ |
| G2 | 自動切り替え | `updateAutoPrinterMode()`（loop から、`speaking` 中は何もしない）。`synced` かつ `isActive()` で Online なら MQTT モードへ（`printerModeAuto`・`modeBeforeAuto` を記録）。進行中でなくなって `kAutoModeReturnMs`（5分）で元のモードへ。手動でモードを選ぶ（`selectModeManually()`）と自動の記録を消し、印刷中に MQTT から抜けたら `autoModeHeldOff` でそのジョブ中は入らない。設定 `autoPrinterMode`（NVS `auto_mqtt`） | ✅ |
| G3 | 実況の出し分け | MQTT モード以外では Low の実況を `remember()` だけして喋らない | ✅ |
| G4 | メニュー・Web | メニュー3列（1段目がモード、注記は `lgfxJapanGothicP_12`）。`POST /api/mode` は `pendingModeRequest` に積むだけで loop が切り替える。ダッシュボードにモード表示と切り替えボタン | ✅ |

## Part H: 本体のキーボードで Wi-Fi・MQTT を設定（2026-09-25 / v2.2.0）

| # | 項目 | 内容 | 状態 |
|---|------|------|------|
| H1 | タッチキーボード | `TouchKeyboard`: `runTouchKeyboard(canvas, value, options, service)`（ブロッキング、入力中も `service()`=`serviceApp` を呼ぶ）。英字/記号/数字キーパッドの3ページ、⇧ 1回=次の1文字、2回=大文字固定。⌫ 長押しで連続削除。伏せ字は最後の1文字を1秒見せる。キーは 10 ユニット幅の行で定義（`buildLayout()`） | ✅ |
| H2 | Wi-Fi 画面 | `runWifiSetup()`: 非同期スキャン（`scanNetworks(true)`）→ SSID ごとに最強の電波でまとめて強い順 → パスワード → `ConfigPortal::testWifi()`（15秒、失敗したら元の Wi-Fi へつなぎ直す）→ `saveWifi()` → 再起動。保存済み SSID でパスワード空なら保存済みを使う | ✅ |
| H3 | プリンター画面 | `runPrinterSetup()`: 監視・IP（数字キー）・シリアル（大文字固定）・アクセスコード（伏せ字、空なら保存済みを残す）・自動 MQTT。`saveBambu()` → 再起動 | ✅ |
| H4 | 組み込み | メニュー3×3に「Wi-Fi」「MQTT 設定」。`openDeviceSetup()` はメニューから顔の描画を止めたまま画面を切り替え、`deviceSetupOpen` の間は首を動かさない。セットアップ AP 中は loop が `handleSetupPortalTouch()`（タップで Wi-Fi 画面）。SSID 未設定の初回起動は自動で Wi-Fi 画面 | ✅ |
| H5 | 共通部品 | `SetupUi.h`: 色・`Button`・`TouchTracker`（押下表示と離したときの確定）・`fitText()` | ✅ |

## Part I: 内蔵ボイスでどんな文章も読む（2026-09-25 / v2.3.0）

| # | 項目 | 内容 | 状態 |
|---|------|------|------|
| I1 | 読み辞書 | `tools/make_dict.py` → `dict/ja.dic`（naist-jdic、固有名詞は地域・一般・姓のみ、まれな活用形は除く、1表層形8項目まで、64語ずつ raw deflate、連接コストは k-means で 256×256 に縮約、1字項目の無い漢字は熟語から読みを推定）、`dict/en.dic`（CMUdict × wordfreq 上位12万語のうち決定木で当たらない語とローマ字として読める語、決定木 IGTree 5段 370KB）。`board_build.embed_files` で埋め込み、`_binary_dict_*_start` で参照 | ✅ |
| I2 | 読み上げ前処理 | `src/talk/`（Arduino 非依存）。`TextReader` → `JaReader`（Viterbi・Open JTalk の njd_set_accent_phrase/accent_type 規則・無声化）/`EnReader`/`Numbers`。数・英単語のあとは文頭の右文脈を変えて助数詞・助詞を選ばせる。inflate は S3 の ROM `tinfl_decompress`（PC は miniz）。作業領域は PSRAM（`allocLarge`） | ✅ |
| I3 | モーラの音 | `make_voice_pack.py` がキー `\x01カナ`（低 5.84）/`\x02カナ`（高 6.08）/`\x03カナ`（無声化）を追加。カナ→音素は自前の表（Open JTalk は「アキャア」を キ+ヤ に分けるため解析に頼らない）、子音長は `/mora_length`、フレーム（93.75fps）単位で「ア＋モーラ＋同じ母音」から切り出し、母音の RMS を 2700 にそろえる | ✅ |
| I4 | 再生 | `BuiltinVoice::plan()`: 文ごとにクリップで全部読めれば従来どおり、読めなければ `planSpeech()`。モーラは 60 サンプル（5ms）重ねてつなぐ（`holdTail`/`join`）。ッ は 70ms の間、ー は前の母音の音 | ✅ |
| I5 | 検証 | `tools/talk_test/run.py --eval`（VOICEVOX の kana と比較）: 一般文 147 文でモーラ誤り 1.66%、実況文 137 文で 0.75%、アクセント誤り約 3.5〜3.9%。PC で 0.2〜0.5ms/文。実機は未確認 | ✅ |

## Part J: UI の優先（2026-09-25 / v2.3.1）

| # | 項目 | 内容 | 状態 |
|---|------|------|------|
| J1 | クラッシュ修正 | コアダンプ（`esp_coredump info_corefile`、coredump パーティション 0xFF0000）で `drawKeyboard` → `pushSprite` → `spiEndTransaction` の assert を確認。原因は顔の描画タスクとの LCD 取り合い。`pauseDrawing()` は描画タスクが Blocked（フレーム間の vTaskDelay）になるのを待って `vTaskSuspend`（確認〜停止の間だけメインループの優先度を上げる）。`applyExpression()` は停止中は保留（`expressionPending_`）し `resumeDrawing()` で反映。描画タスクの優先度は 0 | ✅ |
| J2 | メニュー | 3列×2段＋閉じる。「設定」→ `runSettingsMenu()`（Wi-Fi / プリンター / 接続情報）。プリンター未設定で MQTT・プリンターを押すと見出しに案内（`showModeMenuHint`） | ✅ |
| J3 | MQTT モードは UI 優先 | MQTT モードに入るとプリンター詳細画面（`showPrinterScreenIfFree` / `switchMenuToPrinterScreen`）。ほかのモードへ移ると `closePrinterScreen()` | ✅ |

## Part K: 再起動の修正（2026-09-25 / v2.3.2）

| # | 項目 | 内容 | 状態 |
|---|------|------|------|
| K1 | 顔の描画タスクのスタック | コアダンプ（DebugException、drawLoop の pxTopOfStack が pxStack+136）でスタックあふれを確認。`replaceDrawTask()` がライブラリの drawLoop（2KB）をフレーム間で止めて消し、8KB・優先度0の `faceDraw` を作って `m5avatar::drawTaskHandle` を差し替える | ✅ |
| K2 | 監視 | `[heap] ... stack face= loop= mqtt=`（uxTaskGetStackHighWaterMark）。実機 30 分: 再起動なし、ヒープ一定、face の最大使用 約 2.7KB | ✅ |

## Part L: UI軽量化・自由文の音声改善（2026-09-26 / v2.4.0）

最新の詳細・実機検証・制限は `agent.md` 第9章を参照。

- UI: PSRAMキャンバスを8bit（75KiB）へ変更し、配色を調整。プリンター画面は表示値が
  変化した時だけ描き、16行ごとの差分をLCDへ転送（最大5fps）。字幕折り返しをキャッシュ。
- 顔: 最大20fps。ライブラリはLCD転送中にもBlockedになるため、Part J/Kの待機状態判定を
  フレーム全体のミューテックスへ置換。初期化中に元の2KB描画タスクを走らせず8KBへ差し替える。
- 音声: 共通の `SpeechSynth` でPSOLAによる音程・母音長の調整、句末の伸び、疑問文の上昇、
  長音の母音継続。既存の辞書・パックと互換。全単語の正読・自然さや英語の母語話者発音は保証しない。
- 内部RAM: 音声バッファ9KiBをPSRAMへ移動。静的RAM66,048B、Flash5,203,529B。
- 検証: 波形テスト4サンプルレートPASS、読み284文の回帰なし、COM4書込み、MQTT監視中の
  自由文再生を確認。音の聴覚評価とタッチUIの目視確認は未実施。

## Part M: 接続後の首の急動作修正（2026-09-26 / v2.4.1）

- BSPのReadPos失敗(-1)が左端(-1280)へ変換される問題を修正。
  `tools/patch_stackchan_bsp.py` がビルド時に固定BSPへ自動適用する。
- LOCAL LLM/MQTT間の切り替えでは動作中の首制御を引き継ぐ。
  位置の自動同期は動作開始時のみとし、80msごとのUART再同期をやめた。
- `tools/motion_test/run.py --compare-v240` で旧版を再現し、修正後の回帰テストにPASS。
  詳細と実機確認は `agent.md` 第10章を参照。

## Part N: 撫でたときの反応（2026-09-26 / v2.5.0）

- `PetReaction` が頭部3領域の移動とBSPのスワイプを統合。6秒間の反応を延長し、
  連続すると3段階で喜ぶ。静止タッチとは区別し、撫でた後のクリック・長押しを抑制。
- 笑顔・ピンクLED・首振りとうなずき・8種類の返事。発話中もタッチを取得して首を動かす。
  内蔵音声を優先。実況OFFでは無音、LEVEL HOLDでは首の水平維持を優先する。
- プリンター詳細を一時的に閉じて顔を見せ、終了時に元の画面へ戻す。
  メニュー・設定・校正中は反応を停止。高優先度のプリンター通知は先に処理する。
- サーボ目標を引き継ぎ、校正範囲と更新ごとの移動量を制限。Part Mの再同期対策を維持。
  吹き出しの文字列更新は顔描画と同じミューテックスで保護する。
- `python tools/pet_test/run.py` で実際の状態機械とmainの動作関数をホスト検証。
  詳しい実装・検証と実機確認の範囲は `agent.md` 第13章を参照。

## Part O: 日本語字幕（2026-09-26 / v2.5.1）

- `FaceHud` の字幕表示をプリンターHUDのvisibleから独立させ、全モードの顔画面で描画する。
  `HudMouth` の早期returnも取り除く。既存の日本語16pxフォント・UTF-8折返し・バッファを再利用。
- `AvatarFaceController::showCaption` は標準吹き出しを消し、字幕中は拡大・回転を止める。
  表示終了後に元の変形・通常の呼吸ズームへ戻す。
- 撫でる返事は発話中ずっと保持し、終了後2.5秒表示。発話中の追加の撫で動作で上書きしない。
  起動あいさつ・通常読み上げ・実況も同じ字幕を使用。本文不明のGateway命令は字幕に出さない。
- `tools/screenshots/capture_captions.py` で実際の描画コード・顔パーツ・フォントを使って検証/撮影。
  実機の確認範囲とリリース記録は `agent.md` 第14章を参照。

## 進捗ログ
- 2026-06-27: β3.5.0 に復帰確認（HEAD == β3.5.0, working tree clean）。本ドキュメント作成。
- 2026-06-27: ファームウェア A1–A5 実装・ビルド成功（RAM 18.1%, Flash 18.3%）。
- 2026-06-27: Gateway 実装完了（app.py / ui.py / run.sh / stop.sh / requirements / README）。
  TestClient で 16/16 のスモークテスト合格（認証・設定・/ask・/synthesis・履歴・revoice・削除）。
- 2026-06-28: ユーザ報告「全状況でしばらく動かすと落ちる」を調査→アバターの毎フレーム77KB
  スプライト確保が断片化要因と特定し colorDepth=1 へ修正(C1)。診断表示(C2)、漢字読み整形(C3)、
  ピッチ既定上げ(C4)、口/呼吸の誇張(C5)、カメラ目線(C6) を実装。ファーム build 成功
  （RAM 19.5%, Flash 19.1%）。Gateway py_compile OK。
- 2026-06-28: 設定画面に Local LLM Chat UI(D1) を追加（ブラウザ直 fetch、ESP32中継なし）。
  ゲーミングRGB(D2–D4) を実装：顔(画面)＋本体LEDを虹色循環、設定ON/OFF・NVS保存。
  ファーム build 成功（RAM 18.2%, Flash 18.4%）。**v1.1.5（Gaming RGB Edition）としてリリース**。
  README.md / CLAUDE.md / CHANGELOG.md を更新。
- 2026-09-24: stackchan-mqtt として分離。Bambu Lab P1S の LAN MQTT 監視と実況（Part E）を実装。
  ファーム build 成功（RAM 19.5%, Flash 22.8%）。Gateway `__SAY__` は TestClient で確認。
- 2026-09-24: 内蔵ボイス（Part F）。VOICEVOX でボイスパックを作り LittleFS へ焼き、TTS サーバーなしで実況できるようにした。
- 2026-09-25: 印刷開始で MQTT モードへ自動切り替え（Part G）。ビルド成功（RAM 21.9%, Flash 52.6% / app 3MB）。v2.1.0 プレリリース。
- 2026-09-25: 本体のタッチキーボードで Wi-Fi・MQTT を設定（Part H）。画面は PIL のモックで配置を確認。ビルド成功（Flash 53.6%）。v2.2.0 プレリリース。
- 2026-09-25: 内蔵ボイスでどんな文章も読む（Part I）。ビルド成功（Flash 5.19MB / 5.75MB）。v2.3.0 プレリリース。
- 2026-09-25: 実機に v2.3.0 を書き込み（free text on、自由な文章の読み上げを確認）。ユーザー報告の設定画面クラッシュ・画面のちらつき・メニューの重複を修正（Part J）。v2.3.1 プレリリース。
- 2026-09-25: ユーザー報告「ときどき再起動」を実機で監視しながら調査。顔の描画タスクのスタックあふれを修正（Part K）。v2.3.2 プレリリース。
