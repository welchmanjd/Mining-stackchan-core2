# Mining Stackchan (M5Stack Core2)

Mining Stackchan は、M5Stack Core2 上で **控えめに Duino-Coin をマイニングしながら**、スタックチャン（M5Stack-Avatar）の挙動を眺めることを主目的としたプロジェクトです。

<p align="center">
  <img src="images/demo-avatar-mode.mp4" width="400">
  <img src="images/demo-dashboard-mode.mp4" width="400">
  <br>
  <em>左：スタックチャンモード / 右：ダッシュボードモード</em>
</p>

v0.50 では内部構造を整理し、「喋る / 黙る / 反応する」が破綻しない、落ち着いた振る舞いを目指しました。

## Features

- **スタックチャンの表示と挙動**: [M5Stack-Avatar](https://github.com/meganetaaan/m5stack-avatar) による愛らしい動き
- **Duino-Coin マイニング**: Core2 のリソースを活かした低負荷マイニング
- **Azure TTS による音声発話**: マイニング状況やイベントに応じた発話（イベント駆動）
- **v0.50 安定性向上**: 内部設計を整理し、動作の安定性を追求

> [!NOTE]
> 本プロジェクトは収益を目的としたものではありません。

## Notes

- 電気代・デバイスの寿命などは自己責任でお願いします。
- Duino-Coin 側の仕様変更により動作しなくなる可能性があります。

## Hardware

- M5Stack Core2 (for AWS など、PSRAM 搭載モデル推奨)

## Build / Setup (PlatformIO)

1. **環境準備**: PlatformIO を使用します。
2. **設定の作成**: `config_private.sample.h` を `config_private.h` にコピーしてください。
3. **パラメータ設定**: `config_private.h` 内の以下項目を設定します。
   - Wi-Fi 情報
   - Duino-Coin ユーザー名
   - Azure TTS（エンドポイント / キー）
4. **ビルド＆書き込み**: PlatformIO の環境 `m5stack-core2` を選択してください。

※ 現在 WIP (Work In Progress) のため、仕様が変更される可能性があります。

## Controls

- **BtnA**: ダッシュボード画面 ⇔ スタックチャン画面 の切り替え
- **画面タップ**: Attention モード（短時間）
- **BtnB**: テスト発話
- **BtnC**: テストモード ON / OFF

## Architecture (v0.50)

State / Detect / Decide / Present を分離し、UI と TTS を直接結合しない設計を採用しています。

- `mining_task.*`: マイニング処理と統計更新
- `app_presenter.*`: UI 用データ整形
- `stackchan_behavior.*`: 判断・イベント生成
- `ui_mining_core2.*`: 画面描画
- `azure_tts.*`: TTS 通信・再生

## Logging (v0.50)

- `[EVT]`: イベントの流れを追跡
- `HEARTBEAT`: 生存確認用（低頻度）

## クレジット / 謝辞

- スタックチャンの顔表示には [M5Stack-Avatar](https://github.com/meganetaaan/m5stack-avatar) を使用しています。素晴らしいライブラリに感謝します。
