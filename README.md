# Mining stackchan (M5Stack Core2)

M5Stack Core2 上で Duino-Coin をマイニングしながら、
M5Stack-Avatar でスタックチャンの顔を表示するプロジェクトです。

## Hardware

- M5Stack Core2 (for AWS など、PSRAM 搭載モデル推奨)

## Build

- PlatformIO を使用
- `config_private.sample.h` を `config_private.h` にコピーして、
  Wi-Fi や Duino-Coin の設定を書き換えてください。
- その後、`m5stack-core2` 環境をビルド & 書き込みします。

このリポジトリはまだ作業中 (WIP) です。コードや仕様は今後変更される可能性があります。


## Screenshots

![Mining stackchan avatar](images/mining-stackchan-core2_avatar.jpg)

![Mining status screen](images/mining-stackchan-core2_mining.jpg)
