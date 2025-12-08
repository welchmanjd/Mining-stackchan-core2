# Mining stackchan (M5Stack Core2)

M5Stack Core2 上で Duino-Coin をマイニングしながら、
M5Stack-Avatar でスタックチャンの顔を表示するプロジェクトです。

## Features

-かわいさ最優先、スタックチャンの健気な働きっぷりを眺められます
-マイニングをしてはいますが、ほぼ間違いなく収支はマイナスになります。

## Notes

- 電気代・デバイスの寿命などは自己責任でお願いします。
- Duino-Coin 側の仕様変更により動作しなくなる可能性があります。

## Hardware

- M5Stack Core2 (for AWS など、PSRAM 搭載モデル推奨)

## Build

- PlatformIO を使用
- `config_private.sample.h` を `config_private.h` にコピーして、
  Wi-Fi や Duino-Coin の設定を書き換えてください。
- その後、`m5stack-core2` 環境をビルド & 書き込みします。

このリポジトリはまだ作業中 (WIP) です。コードや仕様は今後変更される可能性があります。


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
