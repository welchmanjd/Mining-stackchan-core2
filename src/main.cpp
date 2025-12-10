// src/main.cpp
// ===== Mining-chan Core2 — main entry (UI + orchestrator) =====
// Board   : M5Stack Core2
// Libs    : M5Unified, ArduinoJson, WiFi, WiFiClientSecure, HTTPClient, m5stack-avatar
// Notes   : マイニング処理は mining_task.* に分離。
//           画面描画は ui_mining_core2.h に集約。

#include <M5Unified.h>
#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <stdarg.h>
#include <esp32-hal-cpu.h>

#include "ui_mining_core2.h"
#include "config.h"
#include "mining_task.h"
#include "logging.h"   // ← 他の #include と一緒に、ファイル先頭の方へ移動推奨

// UI 更新用の前回時刻 [ms]
static unsigned long lastUiMs = 0;

// Aボタンで切り替える画面モード
static bool g_stackchanMode = false;

// ===== 自動スリープ関連 =====

// 最後に「ユーザー入力」があった時刻 [ms]
static unsigned long lastInputMs = 0;

// 画面がスリープ（消灯）中かどうか
static bool displaySleeping = false;

// 画面関連の定数
static const uint8_t  DISPLAY_ACTIVE_BRIGHTNESS = 128;     // 通常時の明るさ
static const uint32_t DISPLAY_SLEEP_TIMEOUT_MS  =
    (uint32_t)MC_DISPLAY_SLEEP_SECONDS * 1000UL;           // 設定値[秒]→[ms]で画面OFF

// スリープ前の「Zzz…」表示時間 [ms]
static const uint32_t DISPLAY_SLEEP_MESSAGE_MS  = 5000UL;  // ここを変えれば好きな秒数に

// ---------------- WiFi / Time ----------------
static bool wifi_connect() {
  const auto& cfg = appConfig();

  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.wifi_ssid, cfg.wifi_pass);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(200);
  }
  if (WiFi.status() == WL_CONNECTED) {
    mc_logf("[WIFI] connected: %s", WiFi.localIP().toString().c_str());
    return true;
  }
  mc_logf("[WIFI] connect failed");
  return false;
}

static void setupTimeNTP() {
  setenv("TZ", "JST-9", 1);
  tzset();
  configTime(9 * 3600, 0,
             "ntp.nict.jp",
             "time.google.com",
             "pool.ntp.org");
}

// ---------------- Arduino entry points ----------------
void setup() {
  setCpuFrequencyMhz(240);

  auto cfg_m5 = M5.config();
  cfg_m5.output_power  = true;
  cfg_m5.clear_display = true;
  M5.begin(cfg_m5);

  Serial.begin(115200);

  M5.Display.setBrightness(128);
  M5.Display.fillScreen(BLACK);
  M5.Display.setTextColor(WHITE, BLACK);

  const auto& cfg = appConfig();

  // ★ ここでいきなりUI起動 & スプラッシュ表示
  UIMining::instance().begin(cfg.app_name, cfg.app_version);
  lastUiMs = 0;

  // UIタイマーとスリープタイマー初期化
  lastUiMs        = 0;
  lastInputMs     = millis();
  displaySleeping = false;

  // そのあとログを流しつつ接続処理
  mc_logf("%s %s booting...", cfg.app_name, cfg.app_version);

  wifi_connect();
  setupTimeNTP();

  // FreeRTOS タスクでマイニング開始
  startMiner();
}



void loop() {
  M5.update();

  unsigned long now = millis();

  // --- 入力検出（ボタン + タッチ） ---
  bool anyInput = false;

  // 物理ボタン
  if (M5.BtnA.wasPressed() || M5.BtnB.wasPressed() || M5.BtnC.wasPressed()) {
    anyInput = true;
  }

  // タッチ入力（押されている間は true）
  auto& tp = M5.Touch;
  if (tp.isEnabled()) {
    auto det = tp.getDetail();
    if (det.isPressed()) {
      anyInput = true;
    }
  }

  // --- スリープ中の復帰処理 ---
  if (displaySleeping) {
    if (anyInput) {
      // 何か入力があったら画面ONにして、このフレームは「起きるだけ」
      mc_logf("[MAIN] display wake (sleep off)");
      M5.Display.setBrightness(DISPLAY_ACTIVE_BRIGHTNESS);
      displaySleeping = false;
      lastInputMs     = now;
    }

    delay(2);
    return;
  }


  // ここから「画面がON」の時の処理

  if (anyInput) {
    lastInputMs = now;
  }

  // --- Aボタン：画面モード切り替え + ビープ ---
  // （スタックチャン画面に入る時も必ずピッと鳴る）
  if (M5.BtnA.wasPressed()) {
    M5.Speaker.tone(1500, 50);  // 他の場所と同じ 1500Hz / 50ms

    g_stackchanMode = !g_stackchanMode;
    mc_logf("[MAIN] BtnA pressed, stackchanMode=%d", (int)g_stackchanMode);

    UIMining& ui = UIMining::instance();
    if (g_stackchanMode) {
      ui.onEnterStackchanMode();
    } else {
      ui.onLeaveStackchanMode();
    }
  }

  // --- UI 更新（100ms ごとに1回） ---
  if (now - lastUiMs >= 100) {
    lastUiMs = now;

    MiningSummary summary;
    updateMiningSummary(summary);

    UIMining& ui = UIMining::instance();
    UIMining::PanelData data;

    data.hr_kh     = summary.total_kh;
    data.accepted  = summary.accepted;
    data.rejected  = summary.rejected;

    data.rej_pct   = (summary.accepted + summary.rejected)
                       ? (100.0f * summary.rejected /
                          (float)(summary.accepted + summary.rejected))
                       : 0.0f;

    data.bestshare = -1.0f;
    data.poolAlive = summary.anyConnected;
    data.diff      = (float)summary.maxDifficulty;

    data.ping_ms   = summary.maxPingMs;

    data.elapsed_s = ui.uptimeSeconds();
    data.sw        = appConfig().app_version;
    data.fw        = ui.shortFwString();
    data.poolName  = summary.poolName;
    data.worker    = appConfig().duco_rig_name;

    String ticker = summary.logLine40;
    ticker += " | POOL ";
    ticker += summary.poolName;
    ticker += " | A ";
    ticker += String(summary.accepted);
    ticker += " R ";
    ticker += String(summary.rejected);
    ticker += " | HR ";
    ticker += String(summary.total_kh, 1);
    ticker += "kH/s | D ";
    ticker += String(summary.maxDifficulty);

    // ★ ここで画面を切り替え
    if (g_stackchanMode) {
      ui.drawStackchanScreen(data);   // スタックチャン画面
    } else {
      ui.drawAll(data, ticker);       // ダッシュボード画面
    }
  }

  // --- 一定時間無操作なら画面OFF（マイニングは継続） ---
  if (!displaySleeping && (now - lastInputMs >= DISPLAY_SLEEP_TIMEOUT_MS)) {
    mc_logf("[MAIN] display sleep (screen off)");

    // 右パネルだけ「Zzz…」メッセージを1秒だけ表示
    UIMining::instance().drawSleepMessage();
    delay(DISPLAY_SLEEP_MESSAGE_MS);  // ★ ここを定数に

    M5.Display.setBrightness(0);
    displaySleeping = true;
  }



  delay(2);
}
