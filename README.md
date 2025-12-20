# Mining Stackchan (M5Stack Core2)

Mining Stackchan は、M5Stack Core2 上で **控えめに Duino-Coin をマイニングしながら**、
スタックチャン（M5Stack-Avatar）の挙動を眺めることを主目的としたプロジェクトです。

v0.50 では内部構造を整理し、
「喋る / 黙る / 反応する」が破綻しない、落ち着いた振る舞いを目指しました。


## Features

- スタックチャンの表示と挙動（M5Stack-Avatar）
- Duino-Coin マイニング（Core2 / 低負荷）
- Azure TTS による音声発話（イベント駆動）
- v0.50: 内部設計の整理と安定性向上

※ 本プロジェクトは収益目的ではありません。


## Notes

- 電気代・デバイスの寿命などは自己責任でお願いします。
- Duino-Coin 側の仕様変更により動作しなくなる可能性があります。


## Hardware

- M5Stack Core2 (for AWS など、PSRAM 搭載モデル推奨)


## Build / Setup (PlatformIO)

- PlatformIO を使用します
- 設定ファイルをコピーします:

```
config_private.sample.h
↓ コピー
config_private.h
```

- `config_private.h` に以下を設定してください:
  - Wi-Fi
  - Duino-Coin（ユーザー名など）
  - Azure TTS（エンドポイント / キー）

- PlatformIO の環境 `m5stack-core2` でビルド & 書き込み

このリポジトリはまだ作業中 (WIP) です。コードや仕様は今後変更される可能性があります。


## Controls

- BtnA : ダッシュボード画面 ⇔ スタックチャン画面
- 画面タップ（スタックチャン画面）: Attention モード（短時間）
- BtnB : テスト発話
- BtnC : テストモード ON / OFF


## Architecture (v0.50)

- State / Detect / Decide / Present の分離
- UI と TTS を直接結合しない
- 「喋る / 黙る / 反応する」を壊さない設計

主な役割:

- `mining_task.*` : マイニング処理と統計更新
- `app_presenter.*` : UI 用データ整形
- `stackchan_behavior.*` : 判断・イベント生成
- `ui_mining_core2.*` : 画面描画
- `azure_tts.*` : TTS 通信・再生


## Logging (v0.50)

- `[EVT]` : イベントの流れを追うためのログ
- `HEARTBEAT` : 生存確認用（低頻度）

目的は「今どこで止まっているか」を追えることです。


## クレジット / 謝辞

- スタックチャンの顔表示には [M5Stack-Avatar](https://github.com/meganetaaan/m5stack-avatar) を使用しています。  
  素晴らしいライブラリを公開してくださっている作者のみなさんに感謝します。


## Screenshots

<p>
  <img src="images/mining-stackchan-core2_avatar.jpg"
       alt="Mining stackchan avatar"
       width="400">
</p>

<p>
  <img src="images/mining-stackchan-core2_mining.jpg"
       alt="Mining status screen"
       width="400">
</p>
