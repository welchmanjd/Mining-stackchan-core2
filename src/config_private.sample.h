// src/config_private.sample.h
#pragma once

// このファイルを config_private.h という名前でコピーして、中身を書き換えて使ってください。

//wifi
#define MC_WIFI_SSID      "your-ssid"
#define MC_WIFI_PASS      "your-password"

//Duino-coin
#define MC_DUCO_USER      "your-duco-user"
#define MC_DUCO_MINER_KEY "None"  // ある場合

// 無操作スリープまでの時間（秒）
// 画面だけOFFにして、マイニングは継続します
#define MC_DISPLAY_SLEEP_SECONDS 600

// Azure Speech (TTS)
// Speech Service の「キー」と「リージョン」を設定してください。
// voice は例です。好きな Neural Voice に差し替え可能。
#define MC_AZ_SPEECH_REGION "japaneast"
#define MC_AZ_SPEECH_KEY    "your-azure-speech-key"
#define MC_AZ_TTS_VOICE     "ja-JP-AoiNeural"

// Attentionモードで表示するデフォルト吹き出し（ボタンA / タップ）
#define MC_ATTENTION_TEXT  "Hi there!"

// ★追加：デフォルトのセリフ（必要なら自由に差し替え）
// ShareAccepted（シェア獲得時）
#define MC_SPEECH_SHARE_ACCEPTED "掘れたよ！"
// 起動テスト等（「こんにちはマイニングスタックチャンです」）
#define MC_SPEECH_HELLO "こんにちはマイニングスタックチャンです"

// 任意：あれば：Speech リソースのカスタムサブドメイン
// 例) "my-speech-app"  または  "my-speech-app.cognitiveservices.azure.com"
#define MC_AZ_CUSTOM_SUBDOMAIN ""

// ★追加：CPU動作周波数（MHz）
// 80 / 160 / 240 あたりが代表値
#define MC_CPU_FREQ_MHZ 240

