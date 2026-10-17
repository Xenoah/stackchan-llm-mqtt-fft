# StackChan Local LLM Gateway (Android Termux)

スマホ/PC ブラウザから質問を入力 → Android 内ローカル LLM で回答生成 →
espeak-ng で音声生成 → StackChan に喋らせる中継サーバ。音声認識(STT)は使わない。

全体仕様はリポジトリ直下の [`CLAUDE.md`](../CLAUDE.md) を参照。

## 構成

```
ブラウザ ──(質問)──▶ Gateway(FastAPI :50021) ──▶ llama-server(:8080)
                          │                          回答テキスト
                          ├─ espeak-ng で WAV 生成
                          ├─ current.wav / history 保存
                          └─(POST /api/speak)──▶ StackChan ──(POST /synthesis)──▶ Gateway が WAV を返す
```

## セットアップ（Termux）

```bash
pkg update -y
pkg install -y git cmake clang make python python-pip curl jq espeak-ng nano tmux termux-api coreutils procps
python -m pip install --upgrade pip setuptools wheel
python -m pip install -r requirements.txt
```

### llama.cpp（llama-server）

```bash
git clone https://github.com/ggml-org/llama.cpp ~/llama.cpp
cd ~/llama.cpp && cmake -B build && cmake --build build --config Release -j
mkdir -p ~/models   # ~/models/model.gguf を配置
```

## 起動 / 停止

```bash
./run.sh     # llama-server + Gateway を tmux で起動（termux-wake-lock）
./stop.sh    # 両方停止し wake-lock 解放
```

- `LLAMA_BIN` / `MODEL` 環境変数で llama-server バイナリとモデルのパスを上書き可能。
- ログ: `~/stackchan_gateway/logs/{llama,gateway}.log`
- セッション確認: `tmux attach -t stackchan_gateway`（`Ctrl-b d` でデタッチ）

## 使い方

1. `./run.sh` で起動。
2. 同一 LAN のブラウザで `http://<スマホのLAN内IP>:50021` を開く。
3. 初期パスワード **`stackchan`** でログイン（設定画面で変更可）。
4. 「接続先 / プロンプト」で StackChan の Host/Port を設定して保存。
5. 質問を入力 →「送信して喋らせる」。`auto_speak` ON なら自動で StackChan が喋る。
   声を変える場合は Gateway UI の「音声設定」→「TTS音声」を選ぶ。

### StackChan 側の設定（Web UI: `http://<StackChanのIP>/`）

| 項目 | 値 |
|------|----|
| TTS Engine Type | `simple_wav` |
| TTS Host | このスマホの LAN 内 IP |
| TTS Port | `50021` |
| Speaker / Style ID | `3` |
| Text to speak | `__REASK_LAST__` |

- A ボタン: 最後の質問を**再度 LLM 処理**して喋る（`__REASK_LAST__`）。
- ブラウザ送信: A ボタン不要でそのまま喋る（Gateway が `/api/speak` を叩く）。
- `simple_wav` では音声生成は Gateway 側で行うため、StackChan 側の `Speaker / Style ID`
  を変えても声色は変わらない。声色は Gateway UI の「TTS音声」で変更する。

### プリンター実況（stackchan-mqtt）

StackChan の TTS Engine Type を `simple_wav` にしてプリンター監視を有効にすると、
StackChan は実況文を `__SAY__` 接頭辞つきで `POST /synthesis` に送ります
（例: `__SAY__印刷完了！おつかれさま！`）。Gateway は LLM を使わず、現在の音声設定
（TTS音声・ピッチ・音量・速度・漢字→かな変換）でその文章を WAV にして返します。
生成した音声は `~/stackchan_gateway/say.wav` に上書き保存され、履歴には残りません。

```bash
curl -X POST http://127.0.0.1:50021/synthesis \
  -H "Content-Type: text/plain; charset=utf-8" \
  --data "__SAY__ベッドを温めてるよ" --output say.wav
```

## 保存データ

```
~/stackchan_gateway/
  settings.json     # 設定（パスワードは salt+sha256 ハッシュ）
  current.json      # 現在の発話対象メタ
  current.wav       # 現在の発話 WAV
  say.wav           # 直近のプリンター実況 WAV（__SAY__）
  history/          # 履歴 {id}.json / {id}.wav
  logs/             # llama.log / gateway.log
```

## 主なエンドポイント

| メソッド | パス | 用途 |
|---|---|---|
| GET | `/` | UI（未ログインはログイン画面） |
| POST | `/login` `/logout` | 認証 |
| GET | `/status` | Gateway/LLM/StackChan 疎通・設定・履歴数 |
| POST | `/settings` | 設定保存（パスワード変更含む） |
| POST | `/ask` | 質問 → LLM → WAV → 保存 →（auto_speak）発話 |
| POST | `/voice/rebuild` | 音声だけ再生成 |
| GET | `/current.wav` | 現在の WAV（試聴用） |
| POST | `/synthesis` | **StackChan 専用**。`__REASK_LAST__` で再 LLM、`__SAY__<文章>` で文章をそのまま読み上げ（プリンター実況）、それ以外は current.wav |
| POST | `/stackchan/speak` | StackChan の `/api/speak` を呼ぶ |
| GET | `/history` | 履歴一覧(JSON) |
| POST | `/history/{id}/speak` | 履歴を current にして発話 |
| POST | `/history/{id}/revoice` | 履歴の音声だけ再生成 |
| POST | `/history/{id}/delete` | 履歴削除 |

## トラブルシュート

- **LLM: NG** … `tmux attach -t stackchan_llama` でログ確認。モデルパス/メモリ不足を疑う。
- **StackChan: NG** … Host/Port、同一 LAN か、StackChan 側 TTS Host が本機 IP かを確認。
- **「Japanese letter」「Chinese」のように読まれる** … Gateway UI の
  「回答/読み上げ言語」を `日本語` にし、`kanji_to_kana` を ON にする。
  それでも漢字読みが崩れる場合は `python -m pip install -r requirements.txt` で
  `pykakasi` が入っているか確認する。
- **声色を変えたい** … Gateway UI の「音声設定」→「TTS音声」を変更してから
  「音声だけ再生成」または「送信して喋らせる」を実行する。
- **busy** … StackChan が発話中。終わってから再送。
