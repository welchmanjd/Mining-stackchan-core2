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
#include <ArduinoJson.h>


#include "ui_mining_core2.h"
#include "app_presenter.h"
#include "config.h"
#include "mining_task.h"
#include "logging.h"   // ← 他の #include と一緒に、ファイル先頭の方へ移動推奨
#include "mc_config_store.h"

#include "azure_tts.h"
#include "stackchan_behavior.h"
#include "orchestrator.h"
#include "runtime_features.h"

// Azure TTS
static AzureTts g_tts;


// UI 更新用の前回時刻 [ms]
static unsigned long lastUiMs = 0;

// 画面モードと切り替え
enum AppMode : uint8_t {
  MODE_DASH = 0,
  MODE_STACKCHAN = 1,
};

static AppMode g_mode = MODE_DASH;


// "Attention" ("WHAT?") mode: short-lived focus state triggered by tap in Stackchan screen.
static bool     g_attentionActive = false;
static uint32_t g_attentionUntilMs = 0;
static MiningYieldProfile g_savedYield = MiningYieldNormal();
static bool     g_savedYieldValid = false;


// ===== Web setup serial commands (simple line protocol) =====
// Web側から 1行コマンドを送り、本体が @ で始まる1行レスポンスを返す。
// 例: "HELLO\n" -> "@OK HELLO"

static char   g_setupLine[512];
static size_t g_setupLineLen = 0;

// Read display_sleep_s from mc_config_store JSON (@CFG相当) with fallback
static long getDisplaySleepSecondsFromStore_(long fallbackSec) {
  String j = mcConfigGetMaskedJson(); // contains display_sleep_s, attention_text, etc.
  StaticJsonDocument<1024> doc;
  DeserializationError e = deserializeJson(doc, j);
  if (e) return fallbackSec;

  JsonVariant v = doc["display_sleep_s"];
  if (v.is<long>()) {
    long sec = v.as<long>();
    if (sec > 0) return sec;
  } else if (v.is<int>()) {
    long sec = (long)v.as<int>();
    if (sec > 0) return sec;
  }
  return fallbackSec;
}

// display sleep timeout [ms] (runtime configurable via SET display_sleep_s)
static uint32_t g_displaySleepTimeoutMs = (uint32_t)MC_DISPLAY_SLEEP_SECONDS * 1000UL;

static void handleSetupLine(const char* line) {
  // 空行は無視
  if (!line || !*line) return;

  // コマンドは大文字小文字ゆるく（必要なら厳格にしてOK）
  String cmd(line);
  cmd.trim();

  // まずは最小セット
  if (cmd.equalsIgnoreCase("HELLO")) {
    Serial.println("@OK HELLO");
    return;
  }
  if (cmd.equalsIgnoreCase("PING")) {
    Serial.println("@OK PONG");
    return;
  }
  if (cmd.equalsIgnoreCase("HELP")) {
    Serial.println("@OK CMDS=HELLO,PING,GET INFO,HELP");
    return;
  }
  if (cmd.equalsIgnoreCase("GET INFO")) {
    const auto& cfg = appConfig();
    char buf[200];
    snprintf(buf, sizeof(buf),
             "@INFO {\"app\":\"%s\",\"ver\":\"%s\",\"baud\":%d}",
             cfg.app_name, cfg.app_version, 115200);
    Serial.println(buf);
    return;
  }

    if (cmd.equalsIgnoreCase("GET CFG")) {
    String j = mcConfigGetMaskedJson();
    Serial.print("@CFG ");
    Serial.println(j);
    return;
  }

  if (cmd.equalsIgnoreCase("AZTEST")) {
    const RuntimeFeatures features = getRuntimeFeatures();
    if (!features.ttsEnabled) {
      Serial.println("@AZTEST NG missing_required");
      return;
    }
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("@AZTEST NG wifi_disconnected");
      return;
    }
    // Reload runtime Azure config so tests after SET/SAVE work without reboot.
    g_tts.begin(mcCfgSpkVolume());
    bool ok = g_tts.testCredentials();
    if (ok) Serial.println("@AZTEST OK");
    else    Serial.println("@AZTEST NG fetch_failed");
    return;
  }

  if (cmd.startsWith("SET ")) {
    // SET <KEY> <VALUE>
    String rest = cmd.substring(4);
    int sp = rest.indexOf(' ');
    if (sp < 0) {
      Serial.println("@ERR bad_set_format");
      return;
    }
    String key = rest.substring(0, sp);
    String val = rest.substring(sp + 1);
    key.trim(); val.trim();

    String err;
    if (mcConfigSetKV(key, val, err)) {

      // ---- apply runtime effects immediately (optional but nice) ----
      if (key.equalsIgnoreCase("display_sleep_s")) {
        long sec = val.toInt();
        if (sec > 0) {
          g_displaySleepTimeoutMs = (uint32_t)sec * 1000UL;
        } else {
          g_displaySleepTimeoutMs = (uint32_t)MC_DISPLAY_SLEEP_SECONDS * 1000UL;
        }
        mc_logf("[MAIN] display_sleep_s set: %ld sec => %lu ms",
                sec, (unsigned long)g_displaySleepTimeoutMs);
      }

      if (key.equalsIgnoreCase("attention_text")) {
        UIMining::instance().setAttentionDefaultText(val.c_str());
        mc_logf("[MAIN] attention_text set: %s", val.c_str());
      }

      if (key.equalsIgnoreCase("spk_volume")) {
        int v = val.toInt();
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        M5.Speaker.setVolume((uint8_t)v);
        mc_logf("[MAIN] spk_volume set: %d", v);
      }

      if (key.equalsIgnoreCase("cpu_mhz")) {
        int mhz = val.toInt();
        // mcConfigSetKV 側で 80/160/240 のみ許可している前提
        setCpuFrequencyMhz(mhz);
        mc_logf("[MAIN] cpu_mhz set: %d (now=%d)", mhz, getCpuFrequencyMhz());
      }





      Serial.print("@OK SET ");
      Serial.println(key);

    } else {
      Serial.print("@ERR SET ");
      Serial.print(key);
      Serial.print(" ");
      Serial.println(err);
    }
    return;

  }

  if (cmd.equalsIgnoreCase("SAVE")) {
    String err;
    if (mcConfigSave(err)) Serial.println("@OK SAVE");
    else { Serial.print("@ERR SAVE "); Serial.println(err); }
    return;
  }

  if (cmd.equalsIgnoreCase("REBOOT")) {
    Serial.println("@OK REBOOT");
    Serial.flush();
    delay(100);
    ESP.restart();
    return;
  }



  // 未知コマンド
  Serial.print("@ERR unknown_cmd: ");
  Serial.println(line);
}

static void pollSetupSerial() {
  while (Serial.available() > 0) {
    const char c = (char)Serial.read();
    if (c == '\r') continue;

    if (c == '\n') {
      g_setupLine[g_setupLineLen] = '\0';
      handleSetupLine(g_setupLine);
      g_setupLineLen = 0;
      continue;
    }

    // バッファ満杯なら捨ててERR返す（暴走防止）
    if (g_setupLineLen + 1 >= sizeof(g_setupLine)) {
      g_setupLineLen = 0;
      Serial.println("@ERR line_too_long");
      continue;
    }

    g_setupLine[g_setupLineLen++] = c;
  }
}




static StackchanBehavior g_behavior;
static Orchestrator g_orch;
static uint32_t g_ttsInflightId = 0;
static uint32_t g_ttsInflightRid = 0;
static String   g_ttsInflightSpeechText;
static uint32_t g_ttsInflightSpeechId = 0;
static bool     g_ttsPrevBusy = false;
static bool     g_prevAudioPlaying = false;
static uint32_t g_lastPopEmptyLogMs = 0;
static bool     g_lastPopEmptyBusy = false;
static AppMode  g_lastPopEmptyMode = MODE_DASH;
static bool     g_lastPopEmptyAttn = false;

// ---- Stackchan bubble-only (no TTS) ----
// speak=false で出す吹き出し（一定時間で自動クリア）
static bool     g_bubbleOnlyActive = false;
static uint32_t g_bubbleOnlyUntilMs = 0;
static uint32_t g_bubbleOnlyRid = 0;
static int      g_bubbleOnlyEvType = 0;
// 吹き出し表示時間を文字数に合わせて可変にする
static uint32_t bubbleShowMs(const String& text) {
  const size_t len = text.length();
  // ベース1.5s + 120ms/文字、上限8s（お好みで調整）
  uint32_t ms = 1500 + (uint32_t)(len * 120);
  const uint32_t maxMs = 8000;
  if (ms > maxMs) ms = maxMs;
  return ms;
}

// ReactionPriority -> OrchPrio 変換宣言
static OrchPrio toOrchPrio(ReactionPriority p);

// ===== 自動スリープ関連 =====

// 最後に「ユーザー入力」があった時刻 [ms]
static unsigned long lastInputMs = 0;

// 画面がスリープ（消灯）中かどうか
static bool displaySleeping = false;

// BtnA/B/C の押下に伴う『タッチ開始ビープ』を次のUI更新で1回だけ抑止する
static bool g_suppressTouchBeepOnce = false;


// NTP が一度設定されたかどうか
static bool g_timeNtpDone = false;

// 画面関連の定数
static const uint8_t  DISPLAY_ACTIVE_BRIGHTNESS = 128;     // 通常時の明るさ


// スリープ前の「Zzz…」表示時間 [ms]
static const uint32_t DISPLAY_SLEEP_MESSAGE_MS  = 5000UL;  // ここを変えれば好きな秒数に


// ---- TTS中のマイニング制御（安全側） ----
static int s_baseThreads = -1;      // 通常時のthreadsを記憶
static int s_appliedThreads = -999;
static uint32_t s_zeroSince = 0;

// ---- TTS中のマイニング制御（捨てない pause 版） ----
// ポイント：スレッド数を 0 にしない（JOBを捨てない）
// 再生中だけ「pause」して、終わったら再開する
static bool s_pausedByTts = false;

static void applyMiningPolicyForTts(bool ttsBusy) {
  (void)ttsBusy;

  const bool speaking = M5.Speaker.isPlaying();
  const bool wantPause = speaking;

  if (wantPause != s_pausedByTts) {
    mc_logf("[TTS] mining pause: %d -> %d (speaking=%d)",
            (int)s_pausedByTts, (int)wantPause, (int)speaking);

    setMiningPaused(wantPause);
    s_pausedByTts = wantPause;
  }
}



// ---------------- WiFi / Time ----------------

// WiFi 接続を「状態マシン化」したノンブロッキング版。
// 毎フレーム呼び出される前提で、
//   - 初回呼び出し時に WiFi.begin() をキック
//   - 接続が完了するかタイムアウトしたら true を返す
//   - それまでは false を返す
// ※接続に成功したかどうかは WiFi.status() == WL_CONNECTED で判定する。
static bool wifi_connect() {
  const auto& cfg = appConfig();

  // 状態を static で保持
  enum WifiState {
    WIFI_NOT_STARTED,
    WIFI_CONNECTING,
    WIFI_DONE
  };
  static WifiState   state   = WIFI_NOT_STARTED;
  static uint32_t    t_start = 0;
  static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000UL;

  switch (state) {
    case WIFI_NOT_STARTED: {
      WiFi.mode(WIFI_STA);
      WiFi.begin(cfg.wifi_ssid, cfg.wifi_pass);
      t_start = millis();
      mc_logf("[WIFI] begin connect (ssid=%s)", cfg.wifi_ssid);
      state = WIFI_CONNECTING;
      return false;
    }

    case WIFI_CONNECTING: {
      wl_status_t st = WiFi.status();
      if (st == WL_CONNECTED) {
        mc_logf("[WIFI] connected: %s", WiFi.localIP().toString().c_str());
        state = WIFI_DONE;
        return true;
      }
      if (millis() - t_start > WIFI_CONNECT_TIMEOUT_MS) {
        mc_logf("[WIFI] connect timeout (status=%d)", (int)st);
        state = WIFI_DONE;
        // 「接続試行」としては終わったので true を返す（成功/失敗は WiFi.status() で見る）
        return true;
      }
      // まだ接続試行中
      return false;
    }

    case WIFI_DONE:
    default:
      // 2回目以降は何もしない
      return true;
  }
}


static void setupTimeNTP() {
  setenv("TZ", "JST-9", 1);
  tzset();
  configTime(9 * 3600, 0,
             "ntp.nict.jp",
             "time.google.com",
             "pool.ntp.org");
}





void setup() {
  // --- シリアルとログ（最初に開く） ---
  Serial.begin(115200);
  mcConfigBegin();

  delay(50);
  mc_logf("[MAIN] setup() start");

  // --- CPUクロック ---
  const uint32_t req_mhz = mcCfgCpuMhz(); // LittleFS設定があれば優先
  setCpuFrequencyMhz((int)req_mhz);
  mc_logf("[MAIN] cpu_mhz=%d (req=%lu)", getCpuFrequencyMhz(), (unsigned long)req_mhz);


  // --- M5Unified の設定 ---
  auto cfg_m5 = M5.config();
  cfg_m5.output_power  = true;
  cfg_m5.clear_display = true;

  cfg_m5.internal_imu = false;
  cfg_m5.internal_mic = false;
  cfg_m5.internal_spk = true;
  cfg_m5.internal_rtc = true;

  mc_logf("[MAIN] call M5.begin()");
  M5.begin(cfg_m5);
  mc_logf("[MAIN] M5.begin() done");

  M5.Speaker.setVolume(mcCfgSpkVolume());
  mc_logf("[MAIN] spk_volume=%u", (unsigned)mcCfgSpkVolume());
  
  const auto& cfg = appConfig();

  // Apply display sleep seconds from mc_config_store (via @CFG JSON)
  {
    long sec = getDisplaySleepSecondsFromStore_((long)MC_DISPLAY_SLEEP_SECONDS);
    g_displaySleepTimeoutMs = (uint32_t)sec * 1000UL;
    mc_logf("[MAIN] display_sleep_s=%ld => timeout=%lu ms",
            sec, (unsigned long)g_displaySleepTimeoutMs);
  }

  // Azure TTS 初期化
  g_tts.begin();
  g_orch.init();

  // --- 画面の初期状態 ---
  M5.Display.setBrightness(DISPLAY_ACTIVE_BRIGHTNESS);
  M5.Display.fillScreen(BLACK);
  M5.Display.setTextColor(WHITE, BLACK);

  // ★ UI起動 & スプラッシュ表示
  UIMining::instance().begin(cfg.app_name, cfg.app_version);
  UIMining::instance().setAttentionDefaultText(mcCfgAttentionText());

  // スタックチャン「喋る/黙る」時間設定（単位: ms）
  UIMining::instance().setStackchanSpeechTiming(
    2200, 1200,   // 喋る: 2.2〜3.4秒
    900,  1400    // 黙る: 0.9〜2.3秒
  );

  // タイマー類の初期化
  lastUiMs        = 0;
  lastInputMs     = millis();
  displaySleeping = false;

  mc_logf("%s %s booting...", cfg.app_name, cfg.app_version);

  // FreeRTOS タスクでマイニング開始
  startMiner();
}




void loop() {
  M5.update();

  // Web setup serial commands
  pollSetupSerial();

  const uint32_t now = (uint32_t)millis();
  const RuntimeFeatures features = getRuntimeFeatures();

  // Orchestrator tick (timeout recovery)
  if (g_orch.tick(now)) {
    LOG_EVT_INFO("EVT_ORCH_TIMEOUT_MAIN", "recover=1");
    g_tts.requestSessionReset();
    g_ttsInflightId = 0;
    g_ttsInflightRid = 0;
    g_ttsInflightSpeechId = 0;
    g_ttsInflightSpeechText = "";
    UIMining::instance().setStackchanSpeech("");
  }

  // TTS state update + completion
  g_tts.poll();

  const bool audioPlayingNow = M5.Speaker.isPlaying();
  if (!g_prevAudioPlaying && audioPlayingNow && g_ttsInflightId != 0) {
    g_orch.onAudioStart(g_ttsInflightId);

    if (g_ttsInflightSpeechId != 0 &&
        g_ttsInflightSpeechId == g_ttsInflightId &&
        g_ttsInflightSpeechText.length() > 0) {
      UIMining::instance().setStackchanSpeech(g_ttsInflightSpeechText);
      LOG_EVT_INFO("EVT_PRESENT_SPEECH_SYNC",
                   "tts_id=%lu len=%u",
                   (unsigned long)g_ttsInflightId,
                   (unsigned)g_ttsInflightSpeechText.length());
    }
  }
  g_prevAudioPlaying = audioPlayingNow;

  const bool ttsBusyNow = g_tts.isBusy();

  uint32_t gotId = 0;
  if (g_tts.consumeDone(&gotId)) {
    LOG_EVT_INFO("EVT_TTS_DONE_RX_MAIN",
                 "got=%lu inflight=%lu expect_rid=%lu",
                 (unsigned long)gotId,
                 (unsigned long)g_ttsInflightId,
                 (unsigned long)g_ttsInflightRid);

    bool desync = false;
    const bool ok = g_orch.onTtsDone(gotId, &desync);
    if (ok) {
      LOG_EVT_INFO("EVT_TTS_DONE", "rid=%lu tts_id=%lu",
                   (unsigned long)g_ttsInflightRid, (unsigned long)gotId);

      UIMining::instance().setStackchanSpeech("");
      LOG_EVT_INFO("EVT_PRESENT_SPEECH_CLEAR", "tts_id=%lu", (unsigned long)gotId);

      g_ttsInflightSpeechText = "";
      g_ttsInflightSpeechId = 0;
      g_ttsInflightId = 0;
      g_ttsInflightRid = 0;
    } else {
      LOG_EVT_INFO("EVT_TTS_DONE_IGNORED",
                   "got_tts_id=%lu expected=%lu",
                   (unsigned long)gotId,
                   (unsigned long)g_ttsInflightId);

      if (desync) {
        LOG_EVT_INFO("EVT_ORCH_SPEAK_DESYNC",
                     "got=%lu expect=%lu",
                     (unsigned long)gotId,
                     (unsigned long)g_ttsInflightId);
        g_tts.requestSessionReset();
        g_ttsInflightId = 0;
        g_ttsInflightRid = 0;
      }
    }
  }

  g_behavior.setTtsSpeaking(ttsBusyNow);
  applyMiningPolicyForTts(ttsBusyNow);

  // pending があれば空きタイミングで1件だけ実行
  if (!g_tts.isBusy() && g_ttsInflightId == 0 && g_orch.hasPendingSpeak()) {
    auto pending = g_orch.popNextPending();
    if (pending.valid) {
      const bool ok = g_tts.speakAsync(pending.text, pending.ttsId);
      if (ok) {
        g_ttsInflightId  = pending.ttsId;
        g_ttsInflightRid = pending.rid;
        g_ttsInflightSpeechText = pending.text;
        g_ttsInflightSpeechId = pending.ttsId;
        g_orch.setExpectedSpeak(pending.ttsId, pending.rid);
        LOG_EVT_INFO("EVT_PRESENT_TTS_START",
                     "rid=%lu tts_id=%lu type=pending prio=%d busy=%d mode=%d attn=%d",
                     (unsigned long)pending.rid, (unsigned long)pending.ttsId,
                     (int)pending.prio, 0, (int)g_mode, g_attentionActive ? 1 : 0);
      } else {
        LOG_EVT_INFO("EVT_PRESENT_TTS_PENDING_FAIL",
                     "rid=%lu tts_id=%lu prio=%d mode=%d attn=%d",
                     (unsigned long)pending.rid, (unsigned long)pending.ttsId,
                     (int)pending.prio, (int)g_mode, g_attentionActive ? 1 : 0);
      }
    }
  }

  // Wi-Fi切断検知：keep-alive中のTLSセッションを次回に備えて破棄予約
  static wl_status_t s_prevWifi = WL_IDLE_STATUS;
  const wl_status_t wifiNow = WiFi.status();
  if (s_prevWifi == WL_CONNECTED && wifiNow != WL_CONNECTED) {
    mc_logf("[WIFI] disconnected (status=%d) -> reset TTS session", (int)wifiNow);
    g_tts.requestSessionReset();
  }
  s_prevWifi = wifiNow;

  // --- 入力検出（ボタン + タッチ） ---
  bool anyInput = false;

  const bool btnA = M5.BtnA.wasPressed();
  const bool btnB = M5.BtnB.wasPressed();
  const bool btnC = M5.BtnC.wasPressed();
  if (btnA || btnB || btnC) {
    anyInput = true;
    g_suppressTouchBeepOnce = true;
  }

  // タッチ入力
  static bool prevTouchPressed = false;
  bool touchPressed = false;
  bool touchDown    = false;

  int touchX = 0;
  int touchY = 0;

  auto& tp = M5.Touch;
  static uint32_t s_lastTouchPollMs = 0;
  static int  s_touchX = 0;
  static int  s_touchY = 0;
  static bool s_touchPressed = false;

  if (tp.isEnabled()) {
    if ((uint32_t)(now - s_lastTouchPollMs) >= 25) {
      s_lastTouchPollMs = now;
      auto det = tp.getDetail();
      s_touchPressed = det.isPressed();
      if (s_touchPressed) {
        s_touchX = det.x;
        s_touchY = det.y;
      }
    }

    touchPressed = s_touchPressed;
    touchX = s_touchX;
    touchY = s_touchY;

    touchDown = touchPressed && !prevTouchPressed;
    prevTouchPressed = touchPressed;

    if (touchPressed) anyInput = true;
  }

  // Cache touch state for UI
  {
    UIMining::TouchSnapshot ts;
    ts.enabled = tp.isEnabled();
    ts.pressed = touchPressed;
    ts.down    = touchDown;
    ts.x       = touchX;
    ts.y       = touchY;
    UIMining::instance().setTouchSnapshot(ts);
  }

  // --- スリープ中の復帰処理 ---
  if (displaySleeping) {
    if (anyInput) {
      mc_logf("[MAIN] display wake (sleep off)");
      M5.Display.setBrightness(DISPLAY_ACTIVE_BRIGHTNESS);
      displaySleeping = false;
      lastInputMs     = now;
    }
    delay(2);
    return;
  }

  // ここから「画面がON」の時の処理
  UIMining& ui = UIMining::instance();

  // Bボタン：設定された「こんにちは」を喋る（動作確認用）
  if (btnB) {
    const char* text = appConfig().hello_text;  // ★変更：設定値（config_private.h / Webで上書き可）
    if (features.ttsEnabled) {
      if (!g_tts.speakAsync(text, (uint32_t)0, nullptr)) {
        mc_logf("[TTS] speakAsync failed (busy / wifi / config?)");
      }
    } else {
      UIMining::instance().setStackchanSpeech(text);
      g_bubbleOnlyActive = true;
      g_bubbleOnlyUntilMs = now + bubbleShowMs(String(text));
      g_bubbleOnlyRid = 0;
      g_bubbleOnlyEvType = 0;
    }
  }


  if (anyInput) lastInputMs = now;

  // --- Aボタン：ダッシュボード <-> スタックチャン + ビープ ---
  if (btnA) {
    M5.Speaker.tone(1500, 50);

    if (g_mode == MODE_DASH) {
      g_mode = MODE_STACKCHAN;
      ui.onEnterStackchanMode();
    } else {
      g_mode = MODE_DASH;
      ui.onLeaveStackchanMode();

      if (g_attentionActive) {
        g_attentionActive = false;
        if (g_savedYieldValid) setMiningYieldProfile(g_savedYield);
        ui.triggerAttention(0);
      }
    }

    mc_logf("[MAIN] BtnA pressed, mode=%d", (int)g_mode);
  }

  // --- Attention mode: tap in Stackchan screen ---
  if ((g_mode == MODE_STACKCHAN) && touchDown) {
    const uint32_t dur = 3000;
    mc_logf("[ATTN] enter");

    if (!g_attentionActive) {
      g_savedYield = getMiningYieldProfile();
      g_savedYieldValid = true;
    }

    g_attentionActive = true;
    g_attentionUntilMs = now + dur;

    ui.triggerAttention(dur, nullptr);
    M5.Speaker.tone(1800, 30);

    if (g_bubbleOnlyActive) {
      const uint32_t oldRid = g_bubbleOnlyRid;
      const int oldType = g_bubbleOnlyEvType;

      g_bubbleOnlyActive = false;
      g_bubbleOnlyUntilMs = 0;
      g_bubbleOnlyRid = 0;
      g_bubbleOnlyEvType = 0;

      UIMining::instance().setStackchanSpeech("");

      LOG_EVT_INFO("EVT_PRESENT_BUBBLE_ONLY_CLEAR",
                   "rid=%lu type=%d mode=%d attn=%d reason=attention_start",
                   (unsigned long)oldRid, oldType,
                   (int)g_mode, g_attentionActive ? 1 : 0);
    }
  }

  // Attention timeout
  if (g_attentionActive && (int32_t)(g_attentionUntilMs - now) <= 0) {
    g_attentionActive = false;
    mc_logf("[ATTN] exit");

    if (g_savedYieldValid) setMiningYieldProfile(g_savedYield);
    else setMiningYieldProfile(MiningYieldNormal());

    ui.triggerAttention(0);
  }

  // --- 起動時の WiFi 接続 & NTP 同期（ノンブロッキング） ---
  const bool wifiDone = wifi_connect();
  if (wifiDone && !g_timeNtpDone && WiFi.status() == WL_CONNECTED) {
    setupTimeNTP();
    g_timeNtpDone = true;
  }

  // --- UI 更新（100ms ごとに1回） ---
  if ((uint32_t)(now - lastUiMs) >= 100) {
    lastUiMs = now;

    MiningSummary summary;
    updateMiningSummary(summary);

    // bubble-only auto clear（期限切れ）
    if (g_bubbleOnlyActive && (int32_t)(g_bubbleOnlyUntilMs - now) <= 0) {
      g_bubbleOnlyActive = false;
      g_bubbleOnlyUntilMs = 0;

      if (g_mode == MODE_STACKCHAN && !g_attentionActive) {
        UIMining::instance().setStackchanSpeech("");
      }

      LOG_EVT_INFO("EVT_PRESENT_BUBBLE_ONLY_CLEAR",
                   "rid=%lu type=%d mode=%d attn=%d reason=timeout",
                   (unsigned long)g_bubbleOnlyRid, g_bubbleOnlyEvType,
                   (int)g_mode, g_attentionActive ? 1 : 0);

      g_bubbleOnlyRid = 0;
      g_bubbleOnlyEvType = 0;
    }

    UIMining::PanelData data;
    buildPanelData(summary, ui, data);

    g_behavior.update(data, now);

    StackchanReaction reaction;
    if (g_behavior.popReaction(&reaction)) {
      const bool allowSpeak = features.ttsEnabled;
      if (!allowSpeak && reaction.speak) {
        reaction.speak = false;
      }

      LOG_EVT_INFO("EVT_PRESENT_POP",
                   "rid=%lu type=%d prio=%d speak=%d busy=%d mode=%d attn=%d",
                   (unsigned long)reaction.rid, (int)reaction.evType, (int)reaction.priority,
                   reaction.speak ? 1 : 0, ttsBusyNow ? 1 : 0, (int)g_mode, g_attentionActive ? 1 : 0);

      const bool suppressedByAttention = (g_mode == MODE_STACKCHAN) && g_attentionActive;
      const bool isIdleTick = (reaction.evType == StackchanEventType::IdleTick);

      if (g_mode == MODE_STACKCHAN && !isIdleTick) {
        const bool isBubbleInfo =
          (reaction.evType == StackchanEventType::InfoPool) ||
          (reaction.evType == StackchanEventType::InfoPing) ||
          (reaction.evType == StackchanEventType::InfoHashrate) ||
          (reaction.evType == StackchanEventType::InfoShares);

        if (!reaction.speak && !isBubbleInfo) {
          static bool s_hasLastExp = false;
          static m5avatar::Expression s_lastExp = m5avatar::Expression::Neutral;
          if (!s_hasLastExp || reaction.expression != s_lastExp) {
            ui.setStackchanExpression(reaction.expression);
            s_lastExp = reaction.expression;
            s_hasLastExp = true;
          }
        }
      }

      // ---- bubble-only present (speak=0) ----
      if (g_mode == MODE_STACKCHAN) {
        if (reaction.speak && g_bubbleOnlyActive) {
          g_bubbleOnlyActive = false;
          g_bubbleOnlyUntilMs = 0;

          if (!g_attentionActive) {
            UIMining::instance().setStackchanSpeech("");
          }

          LOG_EVT_INFO("EVT_PRESENT_BUBBLE_ONLY_CLEAR",
                       "rid=%lu type=%d mode=%d attn=%d reason=tts_event",
                       (unsigned long)g_bubbleOnlyRid, g_bubbleOnlyEvType,
                       (int)g_mode, g_attentionActive ? 1 : 0);

          g_bubbleOnlyRid = 0;
          g_bubbleOnlyEvType = 0;
        }

        if (!reaction.speak &&
            !isIdleTick &&
            reaction.speechText.length() &&
            !suppressedByAttention) {

          UIMining::instance().setStackchanSpeech(reaction.speechText);

          g_bubbleOnlyActive = true;
          const uint32_t showMs = bubbleShowMs(reaction.speechText);
          g_bubbleOnlyUntilMs = now + showMs;
          g_bubbleOnlyRid = reaction.rid;
          g_bubbleOnlyEvType = (int)reaction.evType;

          LOG_EVT_INFO("EVT_PRESENT_BUBBLE_ONLY_SHOW",
                       "rid=%lu type=%d prio=%d len=%u mode=%d attn=%d show_ms=%lu text=%s",
                       (unsigned long)reaction.rid,
                       (int)reaction.evType, (int)reaction.priority,
                       (unsigned)reaction.speechText.length(),
                       (int)g_mode, g_attentionActive ? 1 : 0,
                       (unsigned long)showMs,
                       reaction.speechText.c_str());
        }
      }

      // TTS
      if (reaction.speak && reaction.speechText.length() && features.ttsEnabled) {
        auto cmd = g_orch.makeSpeakStartCmd(reaction.rid, reaction.speechText,
                                            toOrchPrio(reaction.priority),
                                            Orchestrator::OrchKind::BehaviorSpeak);
        if (cmd.valid) {
          const bool canSpeakNow = (!ttsBusyNow) && (g_ttsInflightId == 0);
          if (canSpeakNow) {
            const bool speakOk = g_tts.speakAsync(cmd.text, cmd.ttsId);
            if (speakOk) {
              g_ttsInflightId  = cmd.ttsId;
              g_ttsInflightRid = reaction.rid;
              g_ttsInflightSpeechText = cmd.text;
              g_ttsInflightSpeechId = cmd.ttsId;
              g_orch.setExpectedSpeak(cmd.ttsId, reaction.rid);

              LOG_EVT_INFO("EVT_PRESENT_TTS_START",
                           "rid=%lu tts_id=%lu type=%d prio=%d busy=%d mode=%d attn=%d",
                           (unsigned long)reaction.rid, (unsigned long)cmd.ttsId,
                           (int)reaction.evType, (int)reaction.priority,
                           ttsBusyNow ? 1 : 0, (int)g_mode, g_attentionActive ? 1 : 0);
            }
          } else {
            g_orch.enqueueSpeakPending(cmd);
            LOG_EVT_INFO("EVT_PRESENT_TTS_DEFER_BUSY",
                         "rid=%lu tts_id=%lu prio=%d busy=%d mode=%d attn=%d",
                         (unsigned long)reaction.rid, (unsigned long)cmd.ttsId,
                         (int)reaction.priority, ttsBusyNow ? 1 : 0,
                         (int)g_mode, g_attentionActive ? 1 : 0);
          }
        }
      }
    } else {
      // low-rate heartbeat only
      static uint32_t s_lastHbMs = 0;
      static uint32_t s_emptyStreak = 0;
      s_emptyStreak++;

      const uint32_t PRESENTER_HEARTBEAT_MS = 10000;
      const bool stateChanged =
        (ttsBusyNow != g_lastPopEmptyBusy) ||
        (g_mode != g_lastPopEmptyMode) ||
        (g_attentionActive != g_lastPopEmptyAttn);

      if (stateChanged || (now - s_lastHbMs) >= PRESENTER_HEARTBEAT_MS) {
        LOG_EVT_DEBUG("EVT_PRESENT_HEARTBEAT",
                      "busy=%d mode=%d attn=%d empty_streak=%lu",
                      ttsBusyNow ? 1 : 0, (int)g_mode, g_attentionActive ? 1 : 0,
                      (unsigned long)s_emptyStreak);

        s_lastHbMs = now;
        s_emptyStreak = 0;

        g_lastPopEmptyBusy = ttsBusyNow;
        g_lastPopEmptyMode = g_mode;
        g_lastPopEmptyAttn = g_attentionActive;
      }
    }

    String ticker = buildTicker(summary);

    // 画面描画
    if (g_mode == MODE_STACKCHAN) {
      ui.drawStackchanScreen(data);
    } else {
      ui.drawAll(data, ticker);
    }

    // ボタン押下に伴うタッチ開始ビープ抑止（次の draw 1回だけ）
    g_suppressTouchBeepOnce = false;
  }

  // --- 一定時間無操作なら画面OFF（マイニングは継続）---
  if (!displaySleeping && (uint32_t)(now - lastInputMs) >= g_displaySleepTimeoutMs) {
    mc_logf("[MAIN] display sleep (screen off)");
    UIMining::instance().drawSleepMessage();
    delay(DISPLAY_SLEEP_MESSAGE_MS);
    M5.Display.setBrightness(0);
    displaySleeping = true;
  }

  // ---- TTS中のマイニング負荷制御（捨てない版） ----
  static bool s_ttsYieldApplied = false;
  static MiningYieldProfile s_ttsSavedYield = MiningYieldNormal();
  static bool s_ttsSavedYieldValid = false;

  if (g_tts.isBusy()) {
    if (!s_ttsYieldApplied && !g_attentionActive) {
      s_ttsSavedYield = getMiningYieldProfile();
      s_ttsSavedYieldValid = true;
      setMiningYieldProfile(MiningYieldStrong());
      s_ttsYieldApplied = true;
      mc_logf("[TTS] mining yield: Strong");
    }
  } else {
    if (s_ttsYieldApplied && !g_attentionActive) {
      if (s_ttsSavedYieldValid) setMiningYieldProfile(s_ttsSavedYield);
      else setMiningYieldProfile(MiningYieldNormal());
      s_ttsYieldApplied = false;
      mc_logf("[TTS] mining yield: restore");
    }
  }

  delay(2);
}








// ReactionPriority -> OrchPrio 変換
static OrchPrio toOrchPrio(ReactionPriority p) {
  switch (p) {
    case ReactionPriority::Low:    return OrchPrio::Low;
    case ReactionPriority::High:   return OrchPrio::High;
    case ReactionPriority::Normal:
    default:                       return OrchPrio::Normal;
  }
}
