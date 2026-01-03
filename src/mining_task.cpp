// src/mining_task.cpp
#include "mining_task.h"
#include "config.h"
#include "logging.h"   // ★これが必要

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <mbedtls/sha1.h>

#include "runtime_features.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


static volatile bool g_miningPaused = false;

void setMiningPaused(bool paused) {
  g_miningPaused = paused;
}

bool isMiningPaused() {
  return g_miningPaused;
}

// “pause中はここで待つ” ユーティリティ（忙しいループに入れやすい）
static inline void waitWhilePaused_() {
  while (g_miningPaused) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}


// ---------------- Duino-Coin 固定値 ----------------
static const uint8_t DUCO_MINER_THREADS = 2;
static const char*   DUCO_POOL_URL      = "https://server.duinocoin.com/getPool";

// ---------------- 内部状態 ----------------
struct DucoThreadStats {
  bool     connected    = false;
  float    hashrate_kh  = 0.0f;
  uint32_t shares       = 0;
  uint32_t difficulty   = 0;
  uint32_t accepted     = 0;
  uint32_t rejected     = 0;
  float    last_ping_ms = 0.0f;
  // ★追加: SHA1 演出用（実値）スナップショット
  bool     work_valid      = false;
  uint32_t work_nonce      = 0;
  uint32_t work_max_nonce  = 0;
  uint32_t work_diff       = 0;
  uint8_t  work_out[20]    = {0};    // out[20] の生バイト
  char     work_seed[41]   = {0};    // prev（最大40）
};

static DucoThreadStats   g_thr[DUCO_MINER_THREADS];
static SemaphoreHandle_t g_shaMutex = nullptr;

// ★追加：スナップショット共有の排他用（duco_task / solver / updateMiningSummary で共通）
static portMUX_TYPE g_statsMux = portMUX_INITIALIZER_UNLOCKED;

static String   g_node_name;
static String   g_host;
static uint16_t g_port = 0;
static uint32_t g_acc_all = 0, g_rej_all = 0;
static String   g_status = "boot";
static bool     g_any_connected = false;
static char     g_chip_id[16] = {0};
static int      g_walletid = 0;
// ★追加: プールの診断メッセージ（UIに渡す用）
static String   g_poolDiagText = "";

// ===== mining control knobs (for attention mode etc.) =====
static volatile uint8_t  g_mining_active_threads = DUCO_MINER_THREADS; // 0..DUCO_MINER_THREADS
static volatile uint16_t g_yield_every = 1024;   // power-of-two recommended
static volatile uint8_t  g_yield_ms    = 1;      // delay in ms at yield points

static inline uint16_t normalize_pow2(uint16_t v) {
  if (v < 8) v = 8;
  uint16_t p = 1;
  while ((uint16_t)(p << 1) != 0 && (uint16_t)(p << 1) <= v) p <<= 1;
  return p;
}

// Solver abort marker (distinct from "not found")
static const uint32_t DUCO_ABORTED = UINT32_MAX - 1;



// ---------------- プール情報取得 ----------------
static bool duco_get_pool() {
  WiFiClientSecure s;
  s.setInsecure();
  HTTPClient http;
  http.setTimeout(7000);

  if (!http.begin(s, DUCO_POOL_URL)) {
    g_poolDiagText = "Cannot connect to the pool info server.";
    return false;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    g_poolDiagText = "Pool info server responded with an error.";
    return false;
  }

  String body = http.getString();
  http.end();

  JsonDocument doc;  // ArduinoJson v7
  if (deserializeJson(doc, body)) {
    g_poolDiagText = "Failed to parse pool info response.";
    return false;
  }

  g_node_name = doc["name"].as<String>();
  g_host      = doc["ip"].as<String>();
  g_port      = (uint16_t)doc["port"].as<int>();

  mc_logf("[DUCO] Pool: %s (%s:%u)", g_node_name.c_str(),
          g_host.c_str(), (unsigned)g_port);

  if (g_port != 0 && g_host.length()) {
    // ここでは「Pool自体の情報は取得OK」
    g_poolDiagText = "";
    return true;
  }

  g_poolDiagText = "Pool info response is incomplete.";
  return false;
}


// ---------- util: u32_to_dec（固定バッファへ10進変換） ----------
static inline int u32_to_dec(char* dst, uint32_t v) {
  if (v == 0) {
    dst[0] = '0';
    return 1;
  }
  char tmp[10];
  int n = 0;
  while (v) {
    tmp[n++] = char('0' + (v % 10));
    v /= 10;
  }
  for (int i = 0; i < n; ++i) {
    dst[i] = tmp[n - 1 - i];
  }
  return n;
}

// ---------- SHA1 helper (mbedTLS) ----------
static inline void sha1_calc(const unsigned char* data,
                             size_t len,
                             unsigned char out[20]) {
#if defined(MBEDTLS_VERSION_NUMBER) && (MBEDTLS_VERSION_NUMBER >= 0x03000000)
  mbedtls_sha1(data, len, out);
#else
  mbedtls_sha1_ret(data, len, out);
#endif
}

// ---------- solver: duco_s1（mbedTLS SHA1 + 固定バッファ） ----------
// ★変更: stats を渡して「いま計算している out/nonce」をスナップショットする
static uint32_t duco_solve_duco_s1(const String& seed,
                                  const unsigned char* expected20,
                                  uint32_t difficulty,
                                  uint32_t& hashes_done,
                                  DucoThreadStats* stats) {
  const uint32_t maxNonce = difficulty * 100U;
  hashes_done = 0;

  char buf[96];
  int seed_len = seed.length();
  if (seed_len > (int)sizeof(buf) - 12) seed_len = sizeof(buf) - 12;
  memcpy(buf, seed.c_str(), seed_len);
  char* nonce_ptr = buf + seed_len;

  unsigned char out[20];

// thread index (0/1..) for control checks
const int tidx = (stats) ? int(stats - g_thr) : -1;
if (tidx >= 0 && tidx >= (int)g_mining_active_threads) {
  return DUCO_ABORTED;
}

  for (uint32_t nonce = 0; nonce <= maxNonce; ++nonce) {
    // ---- ★ Pause: keep current JOB, stop only the CPU-heavy loop ----
    // When paused, we yield here and resume from the same nonce (no disconnect / no job drop).
    if (g_miningPaused) {
      waitWhilePaused_();
      // If this thread got disabled while paused, abort cleanly.
      if (tidx >= 0 && tidx >= (int)g_mining_active_threads) {
        return DUCO_ABORTED;
      }
    }

    int nlen = u32_to_dec(nonce_ptr, nonce);

    // 修正案：Mutexを外してパフォーマンス増加を期待
    // if (g_shaMutex) xSemaphoreTake(g_shaMutex, portMAX_DELAY); // 削除
    sha1_calc((const unsigned char*)buf, seed_len + nlen, out);
    // if (g_shaMutex) xSemaphoreGive(g_shaMutex);               // 削除

    hashes_done++;

    // ★一致チェック（見つかったら即返す）
    if (memcmp(out, expected20, 20) == 0) {
      if (stats) {
        portENTER_CRITICAL(&g_statsMux);
        stats->work_nonce     = nonce;
        stats->work_max_nonce = maxNonce;
        memcpy(stats->work_out, out, 20);
        stats->work_valid = true;
        portEXIT_CRITICAL(&g_statsMux);
      }
      return nonce;
    }

    // ★一定間隔で「いま計算してる値」をスナップショット + yield + control point
    uint16_t every = g_yield_every;
    uint32_t mask  = (every >= 1) ? (uint32_t)(every - 1) : 0xFFFFFFFFu;
    if ((nonce & mask) == 0) {
      if (stats) {
        portENTER_CRITICAL(&g_statsMux);
        stats->work_nonce     = nonce;
        stats->work_max_nonce = maxNonce;
        memcpy(stats->work_out, out, 20);
        stats->work_valid = true;
        portEXIT_CRITICAL(&g_statsMux);
      }

      // If this thread got disabled mid-job, abort cleanly.
      if (tidx >= 0 && tidx >= (int)g_mining_active_threads) {
        return DUCO_ABORTED;
      }

      uint8_t dms = g_yield_ms;
      if (dms) vTaskDelay(pdMS_TO_TICKS(dms));
    }
  }
  return UINT32_MAX;
}


// ---------------- Miner Task 本体 ----------------
static void duco_task(void* pv) {
  int idx = (int)(intptr_t)pv;
  if (idx < 0 || idx >= DUCO_MINER_THREADS) idx = 0;
  auto& me = g_thr[idx];

  char tag[8];
  snprintf(tag, sizeof(tag), "T%d", idx);
  Serial.printf("[DUCO-%s] miner task start\n", tag);

  const auto& cfg = appConfig();

  for (;;) {
    // ----- mining control: idle if this thread is disabled (STOP/HALF) -----
    if (idx >= (int)g_mining_active_threads) {
      me.connected   = false;
      me.hashrate_kh = 0.0f;
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    // WiFi
    while (WiFi.status() != WL_CONNECTED) {
      // disabled while waiting for WiFi -> just idle
      if (idx >= (int)g_mining_active_threads) {
        me.connected   = false;
        me.hashrate_kh = 0.0f;
        vTaskDelay(pdMS_TO_TICKS(200));
        continue;
      }
      me.connected = false;
      g_status = "WiFi connecting...";
      g_poolDiagText = "Waiting for WiFi connection.";           // ★追加
      vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // Pool
    if (g_port == 0) {
      if (!duco_get_pool()) {
        // duco_get_pool() 内で g_poolDiagText を設定済み
        vTaskDelay(pdMS_TO_TICKS(5000));
        continue;
      }
    }

    WiFiClient cli;
    cli.setTimeout(15);
    Serial.printf("[DUCO-%s] connect %s:%u ...\n",
                  tag, g_host.c_str(), g_port);
    if (!cli.connect(g_host.c_str(), g_port)) {
      me.connected = false;
      g_poolDiagText = "Cannot connect to the pool node.";   // ★追加
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    // banner
    unsigned long t0 = millis();
    while (!cli.available() && cli.connected() && millis() - t0 < 5000) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!cli.available()) {
      cli.stop();
      g_poolDiagText = "Pool node is not responding.";     // ★追加
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }
    String serverVer = cli.readStringUntil('\n');
    g_status       = String("connected (") + tag + ") " + g_node_name;
    serverVer.trim();  // ← ここ追加
    g_poolDiagText = "";                          // ★ここで一旦「エラーなし」に

    // ★ 追加：サーバーバージョンをログ
    mc_logf("[DUCO-%s] server version: %s",
        tag, serverVer.c_str());
    me.connected = true;
    g_status = String("connected (") + tag + ") " + g_node_name;

    // ===== JOB loop =====
    while (cli.connected()) {
      // disabled mid-connection -> disconnect and go idle
      if (idx >= (int)g_mining_active_threads) {
        mc_logf("[DUCO-%s] disabled -> disconnect", tag);
        cli.stop();
        me.connected   = false;
        me.hashrate_kh = 0.0f;
        vTaskDelay(pdMS_TO_TICKS(200));
        break;
      }

      // Request job（user, board, miningKey）
      // Request job（user, board, miningKey）
      // NOTE:
      //   ESP32 を名乗ると Kolka に「Too high starting difficulty」と言われて全シェアがリジェクトされる。
      //   AVR を名乗れば通るが、実際は ESP32 なのでボード名で嘘をつきたくない。
      //   そのため、汎用スタート難易度ラベル "LOW" を指定し、具体的な難易度調整は
      //   サーバー側（Kolka）に任せる方針。
      String req = String("JOB,") + cfg.duco_user + ",LOW," +
                  cfg.duco_miner_key + "\n";

      // ★ 追加：何を投げたか（miner_key はログに出さない）
      mc_logf("[DUCO-%s] send JOB user=%s board=LOW",
              tag, cfg.duco_user);


      unsigned long ping0 = millis();
      cli.print(req);

      // job を待つ
      t0 = millis();
      while (!cli.available() && cli.connected() && millis() - t0 < 10000) {
        vTaskDelay(pdMS_TO_TICKS(10));
      }
      if (!cli.available()) {
        me.connected = false;
        g_status = String("no job (") + tag + ")";

        // ★ 追加：タイムアウトをログ
        mc_logf("[DUCO-%s] no job (timeout)", tag);
        g_poolDiagText = "No job response from the pool."; // ★追加
        break;
      }
      me.last_ping_ms = (float)(millis() - ping0);

      // ★ 追加：ping をログ
      mc_logf("[DUCO-%s] job ping = %.1f ms",
          tag, me.last_ping_ms);

      // job: previousHash,expectedHash,difficulty\n
      String prev     = cli.readStringUntil(',');
      String expected = cli.readStringUntil(',');
      String diffStr  = cli.readStringUntil('\n');
      prev.trim();
      expected.trim();
      diffStr.trim();

      int difficulty = diffStr.toInt();
      if (difficulty <= 0) difficulty = 1;
      me.difficulty = (uint32_t)difficulty;

      // ★追加：演出用スナップショットの“お題”を保存（prev + difficulty）
      portENTER_CRITICAL(&g_statsMux);
      me.work_diff = (uint32_t)difficulty;
      me.work_valid = false;  // 新ジョブ開始で一旦リセット
      strncpy(me.work_seed, prev.c_str(), 40);
      me.work_seed[40] = '\0';
      portEXIT_CRITICAL(&g_statsMux);


     // ★ 追加：ジョブの中身をログ
     mc_logf("[DUCO-%s] job diff=%d prev=%s expected=%s",
          tag, difficulty,
          prev.c_str(), expected.c_str());

      // expected(hex) → 20バイト
      const size_t SHA_LEN = 20;
      unsigned char expBytes[SHA_LEN];
      memset(expBytes, 0, sizeof(expBytes));
      size_t elen = expected.length() / 2;
      const char* ce = expected.c_str();
      auto h = [](char c) -> uint8_t {
        c = toupper((uint8_t)c);
        if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
        if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
        return 0;
      };
      for (size_t i = 0, j = 0; j < elen && j < SHA_LEN; i += 2, ++j) {
        expBytes[j] = (h(ce[i]) << 4) | h(ce[i + 1]);
      }

      // solve
      uint32_t hashes = 0;
      unsigned long tStart = micros();
      uint32_t foundNonce =
          duco_solve_duco_s1(prev, expBytes, (uint32_t)difficulty, hashes, &me);

      if (foundNonce == DUCO_ABORTED) {
        // mining control requested to stop this thread
        mc_logf("[DUCO-%s] job aborted by control", tag);
        cli.stop();
        me.connected   = false;
        me.hashrate_kh = 0.0f;
        vTaskDelay(pdMS_TO_TICKS(200));
        break;
      }

      float sec = (micros() - tStart) / 1000000.0f;
      if (sec <= 0) sec = 0.001f;
      float hps = hashes / (sec > 0 ? sec : 0.001f);

      
      // ★ 追加：solver の実績をログ
      mc_logf("[DUCO-%s] solved nonce=%u hashes=%u time=%.3fs (%.1f H/s)",
            tag,
            (unsigned)foundNonce,
            (unsigned)hashes,
            sec,
            hps);

      if (foundNonce == UINT32_MAX) {
        g_status = String("no share (") + tag + ")";
        vTaskDelay(pdMS_TO_TICKS(5));
        continue;
      }

      me.hashrate_kh = hps / 1000.0f;
      me.shares++;

      // Submit: nonce,hashrate,banner ver,rig,DUCOID<chip>,<walletid>\n
      String submit =
          String(foundNonce) + "," + String(hps) + "," +
          String(cfg.duco_banner) + " " + cfg.app_version + "," +
          cfg.duco_rig_name + "," +
          "DUCOID" + String((char*)g_chip_id) + "," +
          String(g_walletid) + "\n";
      cli.print(submit);

      
      // ★ 追加：送った内容（短く）をログ
      mc_logf("[DUCO-%s] submit nonce=%u hps=%.1f",
              tag, (unsigned)foundNonce, hps);

      // feedback
      t0 = millis();
      while (!cli.available() && cli.connected() && millis() - t0 < 10000) {
        vTaskDelay(pdMS_TO_TICKS(10));
      }
      if (!cli.available()) {
        g_status = String("no feedback (") + tag + ")";

        // ★ 追加：timeout も「失敗したシェア」として数える
        ++me.rejected;
        ++g_rej_all;

        mc_logf("[DUCO-%s] no feedback (timeout)", tag);
        g_poolDiagText = "No result response from the pool."; // ★追加
        break;
      }
      String fb = cli.readStringUntil('\n');
      fb.trim();

      // ★ 追加：フィードバックそのもの
      mc_logf("[DUCO-%s] feedback: '%s'", tag, fb.c_str());

      if (fb.startsWith("GOOD")) {
        ++me.accepted;
        ++g_acc_all;
        g_status = String("share GOOD (#") + String(me.shares) +
                   ", " + tag + ")";
        g_poolDiagText = "";     // ★正常
      } else {
        ++me.rejected;
        ++g_rej_all;
        g_status = String("share BAD (#") + String(me.shares) +
                   ", " + tag + ")";
        // BAD のときはとりあえず直ちにPoolエラー扱いにはしない
      }

      vTaskDelay(pdMS_TO_TICKS(5));
    }

    cli.stop();
    me.connected = false;
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

// ---------------- 公開関数 ----------------
void startMiner() {
  const auto features = getRuntimeFeatures();
  if (!features.miningEnabled) {
    g_status = "disabled";
    g_poolDiagText = "Mining is disabled (Duco user is empty).";
    return;
  }

  g_shaMutex = xSemaphoreCreateMutex();

  uint64_t chipid = ESP.getEfuseMac();
  uint16_t chip   = (uint16_t)(chipid >> 32);
  snprintf((char*)g_chip_id, sizeof(g_chip_id),
           "%04X%08X", chip, (uint32_t)chipid);

  randomSeed((uint32_t)millis());
  g_walletid = random(0, 2811);

  WiFi.setSleep(false);

  for (int i = 0; i < DUCO_MINER_THREADS; ++i) {
    g_thr[i] = DucoThreadStats();
  }
  g_acc_all = g_rej_all = 0;

  for (int i = 0; i < DUCO_MINER_THREADS; ++i) {
    int core = (i == 0) ? 0 : 1;
    UBaseType_t prio = 1;
    String name = String("DucoMiner") + String(i);
    xTaskCreatePinnedToCore(duco_task,
                            name.c_str(),
                            8192,
                            (void*)(intptr_t)i,
                            prio,
                            nullptr,
                            core);
  }
}

// 集計だけ行い、UI に依存しない形で返す
void updateMiningSummary(MiningSummary& out) {
  const auto features = getRuntimeFeatures();

  float    total_kh = 0.0f;
  float    maxPing  = 0.0f;
  uint32_t acc = 0, rej = 0, diff = 0;
  g_any_connected = false;

  for (int i = 0; i < DUCO_MINER_THREADS; ++i) {
    total_kh += g_thr[i].hashrate_kh;
    acc      += g_thr[i].accepted;
    rej      += g_thr[i].rejected;

    if (g_thr[i].difficulty > diff) diff = g_thr[i].difficulty;
    if (g_thr[i].connected) g_any_connected = true;

    if (g_thr[i].last_ping_ms > maxPing) {
      maxPing = g_thr[i].last_ping_ms;
    }
  }

  out.total_kh      = total_kh;
  out.accepted      = acc;
  out.rejected      = rej;
  out.maxDifficulty = diff;
  out.anyConnected  = g_any_connected;
  out.poolName      = g_node_name;
  out.maxPingMs     = maxPing;
  out.miningEnabled = features.miningEnabled;

  char logbuf[64];
  snprintf(logbuf, sizeof(logbuf),
           "%s A%u R%u HR %.1fkH/s d%u",
           g_status.startsWith("share GOOD") ? "good " :
           g_status.startsWith("share BAD")  ? "rej  " :
           g_any_connected ? "alive" : "dead ",
           (unsigned)acc, (unsigned)rej, total_kh, (unsigned)diff);
  out.logLine40 = String(logbuf);

  // ★追加: プール診断メッセージ
  out.poolDiag = g_poolDiagText;
  // ===== 演出用：SHA1(out) スナップショットを summary に詰める =====
  auto hexDigit = [](uint8_t v) -> char {
    return (v < 10) ? (char)('0' + v) : (char)('a' + (v - 10));
  };

  int wi_connected = -1;
  int wi_any = -1;
  for (int i = 0; i < DUCO_MINER_THREADS; ++i) {
    if (g_thr[i].work_valid) {
      if (wi_any < 0) wi_any = i;
      if (g_thr[i].connected && wi_connected < 0) wi_connected = i;
    }
  }
  int wi = (wi_connected >= 0) ? wi_connected : wi_any;

  if (wi >= 0) {
    uint8_t out20[20];
    char seed40[41];
    uint32_t nonce = 0, maxNonce = 0, diffv = 0;

    portENTER_CRITICAL(&g_statsMux);
    nonce   = g_thr[wi].work_nonce;
    maxNonce= g_thr[wi].work_max_nonce;
    diffv   = g_thr[wi].work_diff;
    memcpy(out20, g_thr[wi].work_out, 20);
    strncpy(seed40, g_thr[wi].work_seed, 40);
    seed40[40] = '\0';
    portEXIT_CRITICAL(&g_statsMux);

    out.workThread     = (uint8_t)wi;
    out.workNonce      = nonce;
    out.workMaxNonce   = maxNonce;
    out.workDifficulty = diffv;

    strncpy(out.workSeed, seed40, 40);
    out.workSeed[40] = '\0';

    for (int j = 0; j < 20; ++j) {
      out.workHashHex[j * 2 + 0] = hexDigit((out20[j] >> 4) & 0x0F);
      out.workHashHex[j * 2 + 1] = hexDigit(out20[j] & 0x0F);
    }
    out.workHashHex[40] = '\0';
  } else {
    out.workThread = 255;
    out.workNonce = out.workMaxNonce = out.workDifficulty = 0;
    out.workSeed[0] = '\0';
    out.workHashHex[0] = '\0';
  }

}



// ===== Mining control API (public) =====
void setMiningActiveThreads(uint8_t activeThreads) {
  if (activeThreads > DUCO_MINER_THREADS) activeThreads = DUCO_MINER_THREADS;
  g_mining_active_threads = activeThreads;
}

uint8_t getMiningActiveThreads() {
  return g_mining_active_threads;
}

void setMiningYieldProfile(MiningYieldProfile p) {
  // normalize 'every' to power-of-two (fast bitmask check)
  p.every = normalize_pow2(p.every);
  g_yield_every = p.every;
  g_yield_ms    = p.delay_ms;
}

MiningYieldProfile getMiningYieldProfile() {
  MiningYieldProfile p;
  p.every    = g_yield_every;
  p.delay_ms = g_yield_ms;
  return p;
}
