// src/config_private.sample.h
#pragma once

// このファイルを config_private.h という名前でコピーして、中身を書き換えて使ってください。

//wifi
#define MC_WIFI_SSID      "your-ssid"
#define MC_WIFI_PASS      "your-password"

//Duino-coin
#define MC_DUCO_USER      "your-duco-user"
#define MC_DUCO_MINER_KEY "None"  // ない場合

// 無操作スリープまでの時間（秒）
// 画面だけOFFにして、マイニングは継続します
#define MC_DISPLAY_SLEEP_SECONDS 600

// Azure Speech (TTS)
// Speech Service の「キー」と「リージョン」を設定してください。
// voice は例です。好きな Neural Voice に差し替え可能。
#define MC_AZ_SPEECH_REGION "japaneast"
#define MC_AZ_SPEECH_KEY    "your-azure-speech-key"
#define MC_AZ_TTS_VOICE     "ja-JP-AoiNeural"

// 任意：Speech リソースのカスタムサブドメイン
// 例) "my-speech-app"  または  "my-speech-app.cognitiveservices.azure.com"
#define MC_AZ_CUSTOM_SUBDOMAIN ""
