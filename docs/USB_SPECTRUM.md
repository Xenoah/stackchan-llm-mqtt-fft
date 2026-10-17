# USBオーディオ・FFT v2.6.0

`stackchan-mqtt v2.5.1` に `cores3-usb-spectrum v1.1.0` の解析・画面を統合したファームウェアです。
ファームウェア識別文字列は `v2.6.0`。初回プレリリースです。
公開後の実機検証結果は[リリース説明](https://github.com/Xenoah/stackchan-llm-mqtt-fft/releases/tag/v2.6.0)へ追記します。

## 追加した操作

- USB接続後、PCの音声出力先に **StackChan USB Spectrum** を選びます。
  USB Audio Class 2.0、48 kHz / 16 bit / ステレオで受信し、CoreS3の内蔵モノラルスピーカーへ出力します。
- **ホーム**（従来のLOCAL LLMモード）の顔には、USB入力中だけ32バンドのグラフを表示します。
  入力停止後は約250 msでグラフを消します。顔の色に合わせた表示で、字幕用の下段を残しています。
- 顔の下端から上へスワイプしてメニューを開き、**FFT** を選ぶと専用画面に切り替わります。
- **WAVE / BARS** は波形とスペクトラムを切り替えます。
- **HOLD / RESUME** は表示を保持／再開します。音声再生は継続します。
- **SPK ON / SPK OFF** はUSB音声だけをミュート／解除します。解析と既存の読み上げは継続します。
- 専用画面の上部をタップ、または下端から上へスワイプすると顔へ戻ります。HOLDは解除されます。
- USB再生はメニュー・設定・MQTT画面でも継続します。FFT表示中もMQTT監視とWebからの発話受付を継続します。
  自動MQTT画面切替と頭タッチによる画面操作は、FFTを閉じるまで保留します。

PCのシステム音全体ではなく、PCがこのUSB出力先に送った音を受信します。
専用PCアプリは不要です。初回の出力先選択は必要です。

## 音声の共存

USBはM5Unifiedの仮想チャンネル1、既存の読み上げはチャンネル0を使います。
読み上げ中はUSB音声を約12 dB下げ、終了後に戻します。
本体の保存済みスピーカー音量は両方に適用され、PCの音量・ミュートとFFT画面のSPK操作はUSB側だけに作用します。
読み上げの完了待ちはチャンネル0のみを監視するため、USBが連続再生中でも終了できます。
USBの受信・再生は専用タスク経由で、FFTやLCD描画から独立しています。

## 移植元との差分

- 移植元: [cores3-usb-spectrum v1.1.0](https://github.com/Xenoah/cores3-usb-spectrum/releases/tag/v1.1.0)、コミット `cbd4e71e13d4553233964b993081feb9cf696a06`。
- 4096点Hann窓、25 Hz〜20 kHzの32対数バンド、左右RMS、ピーク周波数、ピーク保持、クリップ表示、左チャンネル波形を維持。
  左右のパワーを合成するため、逆位相の入力でも解析結果は消えません。
- Arduino 2.0.17 / PlatformIO espressif32 6.12.0を維持。USB Audioは同梱TinyUSB 0.16のアプリケーションクラスとして追加し、CDC診断ポートと共存。
- USBはOTGモードへ変更。試作用VID/PIDは `303A:8001`。通常動作時のCOM番号は元のファームウェアと変わる場合があります。
- FFTは移植元のポータブル計算実装を使用。大きな作業バッファはPSRAMに置き、更新を最大約30回/秒に制限して会話・Wi-Fi・顔描画と共存させています。
  元の単機能ファームウェアの134 FPSという実測値は、この統合版の性能を示すものではありません。
- 専用画面は元の差分転送を保持。顔を停止してから専用画面を描き、閉じた後に顔の描画を再開します。
- ホームのグラフは顔と同じスプライトへ描画。LCD競合を避け、グラフ表示中は顔のズーム・回転を抑制します。
- USB音量・HOLD・WAVE設定はセッション内のみ。既存NVSとパーティション構成は変更していません。

## ビルドとPC上の確認

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run
python tools/spectrum_test/run.py
python tools/spectrum_test/run.py --ui
```

`run.py` はPythonの `ziglang`、UI確認はさらに `Pillow` と
`tools/screenshots/capture_device.py` 記載のSDL2開発用ファイルを必要とします。
USB制御テストでは、インストール済みArduinoフレームワークのTinyUSBヘッダーを参照します。
テストはシリアルポートを開かず、PC内だけで実行します。

確認内容:

- ファームウェアのコンパイルとリンク。
- 無音、DC除去、振幅・RMS校正、逆位相、片チャンネル入力、31.25 Hz〜19.95 kHzの周波数推定。
- 実装から生成したUAC2ディスクリプター、CDCとのインターフェース分離、48 kHz固定クロック、音量・ミュート、不正な制御要求の拒否。
- 実際の描画関数とM5GFXフォントを使ったメニューの当たり判定、FFT／波形の差分転送ピクセル比較、HOLD解除、入力停止、再表示、顔グラフと字幕の併存。

[メニュー](screenshots/usb-fft-dev/menu.png) / [ホーム＋字幕](screenshots/usb-fft-dev/home-caption.png) /
[FFT](screenshots/usb-fft-dev/fft-bars.png) / [波形](screenshots/usb-fft-dev/fft-wave.png)

これらの画像は合成音声を使ったPC上の描画結果です。実機写真・実機キャプチャーではありません。
公開後のCoreS3実機確認（2026-10-17）:

- 配布7ファイルのSHA256を照合後、COM4からアプリ領域だけを書き込み。書込みハッシュ照合に合格。
- 起動版 `v2.6.0`、Wi-Fi再接続、MQTT online/synced、内蔵音声パックの再生を確認。
- WindowsにAudioとCDC（COM8）が正常認識。48 kHz/16 bit/ステレオの5秒テスト音と30秒無音ストリームの送信が完了。
- USBストリームを開いたまま内蔵音声の読み上げが完了。
- 60秒間のCDC診断で再起動行・USB切断・例外検出はいずれも0。記録は [validation/v2.6.0.json](validation/v2.6.0.json)。
- Arduino OTGのCDCログはDTR/RTSを両方有効にすると取得できます。従来の `monitor_serial.py` の既定（両方無効）ではログが出ません。

音質の聴感評価・LCDの目視・長時間の音切れ・実機FFTのFPSは未確認です。
`screenshots/usb-fft-dev/verification.json` は開発版をPC内で検証した時点の記録です。
