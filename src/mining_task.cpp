// src/mining_task.cpp
#include "mining_task.h"
#include "config.h"
#include "logging.h"   // ★これが必要

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <mbedtls/sha1.h>




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
};

static DucoThreadStats   g_thr[DUCO_MINER_THREADS];
static SemaphoreHandle_t g_shaMutex = nullptr;

static String   g_node_name;
static String   g_host;
static uint16_t g_port = 0;
static uint32_t g_acc_all = 0, g_rej_all = 0;
static String   g_status = "boot";
static bool     g_any_connected = false;
static char     g_chip_id[16] = {0};
static int      g_walletid = 0;

// ---------------- プール情報取得 ----------------
static bool duco_get_pool() {
  WiFiClientSecure s;
  s.setInsecure();
  HTTPClient http;
  http.setTimeout(7000);
  if (!http.begin(s, DUCO_POOL_URL)) return false;
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  JsonDocument doc;  // ArduinoJson v7
  if (deserializeJson(doc, body)) return false;
  g_node_name = doc["name"].as<String>();
  g_host      = doc["ip"].as<String>();
  g_port      = (uint16_t)doc["port"].as<int>();
  mc_logf("[DUCO] Pool: %s (%s:%u)", g_node_name.c_str(),g_host.c_str(), (unsigned)g_port);
  return g_port != 0 && g_host.length();
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
static uint32_t duco_solve_duco_s1(const String& seed,
                                   const unsigned char* expected20,
                                   uint32_t difficulty,
                                   uint32_t& hashes_done) {
  const uint32_t maxNonce = difficulty * 100U;
  hashes_done = 0;

  char buf[96];
  int seed_len = seed.length();
  if (seed_len > (int)sizeof(buf) - 12) seed_len = sizeof(buf) - 12;
  memcpy(buf, seed.c_str(), seed_len);
  char* nonce_ptr = buf + seed_len;

  unsigned char out[20];
  const uint32_t YIELD_MASK = 0x3FF;  // 1024回ごと

  for (uint32_t nonce = 0; nonce <= maxNonce; ++nonce) {
    int nlen = u32_to_dec(nonce_ptr, nonce);
    if (g_shaMutex) xSemaphoreTake(g_shaMutex, portMAX_DELAY);
    sha1_calc((const unsigned char*)buf, seed_len + nlen, out);
    if (g_shaMutex) xSemaphoreGive(g_shaMutex);
    hashes_done++;
    if (memcmp(out, expected20, 20) == 0) return nonce;
    if ((nonce & YIELD_MASK) == 0) vTaskDelay(1 / portTICK_PERIOD_MS);
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
    // WiFi
    while (WiFi.status() != WL_CONNECTED) {
      me.connected = false;
      g_status = "WiFi connecting...";
      vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    // Pool
    if (g_port == 0) {
      if (!duco_get_pool()) {
        vTaskDelay(5000 / portTICK_PERIOD_MS);
        continue;
      }
    }

    WiFiClient cli;
    cli.setTimeout(15);
    Serial.printf("[DUCO-%s] connect %s:%u ...\n",
                  tag, g_host.c_str(), g_port);
    if (!cli.connect(g_host.c_str(), g_port)) {
      me.connected = false;
      vTaskDelay(1000 / portTICK_PERIOD_MS);
      continue;
    }

    // banner
    unsigned long t0 = millis();
    while (!cli.available() && cli.connected() && millis() - t0 < 5000) {
      vTaskDelay(10 / portTICK_PERIOD_MS);
    }
    if (!cli.available()) {
      cli.stop();
      vTaskDelay(2000 / portTICK_PERIOD_MS);
      continue;
    }
    String serverVer = cli.readStringUntil('\n');
    (void)serverVer;
    me.connected = true;
    g_status = String("connected (") + tag + ") " + g_node_name;

    // ===== JOB loop =====
    while (cli.connected()) {
      // Request job（user, board, miningKey）
      String req = String("JOB,") + cfg.duco_user + ",AVR," +
                   cfg.duco_miner_key + "\n";
      unsigned long ping0 = millis();
      cli.print(req);
      t0 = millis();
      while (!cli.available() && cli.connected() && millis() - t0 < 10000) {
        vTaskDelay(10 / portTICK_PERIOD_MS);
      }
      if (!cli.available()) {
        me.connected = false;
        g_status = String("no job (") + tag + ")";
        break;
      }
      me.last_ping_ms = (float)(millis() - ping0);

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
          duco_solve_duco_s1(prev, expBytes, (uint32_t)difficulty, hashes);
      float sec = (micros() - tStart) / 1000000.0f;
      if (sec <= 0) sec = 0.001f;
      float hps = hashes / (sec > 0 ? sec : 0.001f);

      if (foundNonce == UINT32_MAX) {
        g_status = String("no share (") + tag + ")";
        vTaskDelay(5 / portTICK_PERIOD_MS);
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

      // feedback
      t0 = millis();
      while (!cli.available() && cli.connected() && millis() - t0 < 10000) {
        vTaskDelay(10 / portTICK_PERIOD_MS);
      }
      if (!cli.available()) {
        g_status = String("no feedback (") + tag + ")";
        break;
      }
      String fb = cli.readStringUntil('\n');
      fb.trim();
      if (fb.startsWith("GOOD")) {
        ++me.accepted;
        ++g_acc_all;
        g_status = String("share GOOD (#") + String(me.shares) +
                   ", " + tag + ")";
      } else {
        ++me.rejected;
        ++g_rej_all;
        g_status = String("share BAD (#") + String(me.shares) +
                   ", " + tag + ")";
      }

      vTaskDelay(5 / portTICK_PERIOD_MS);
    }

    cli.stop();
    me.connected = false;
    vTaskDelay(2000 / portTICK_PERIOD_MS);
  }
}

// ---------------- 公開関数 ----------------
void startMiner() {
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
  float    total_kh = 0.0f;
  float    maxPing  = 0.0f;   // ★追加
  uint32_t acc = 0, rej = 0, diff = 0;
  g_any_connected = false;

  for (int i = 0; i < DUCO_MINER_THREADS; ++i) {
    total_kh += g_thr[i].hashrate_kh;
    acc      += g_thr[i].accepted;
    rej      += g_thr[i].rejected;

    if (g_thr[i].difficulty > diff) diff = g_thr[i].difficulty;
    if (g_thr[i].connected) g_any_connected = true;

    if (g_thr[i].last_ping_ms > maxPing) {
      maxPing = g_thr[i].last_ping_ms; // ★追加
    }
  }

  out.total_kh      = total_kh;
  out.accepted      = acc;
  out.rejected      = rej;
  out.maxDifficulty = diff;
  out.anyConnected  = g_any_connected;
  out.poolName      = g_node_name;
  out.maxPingMs     = maxPing;         // ★これでOK

  char logbuf[64];
  snprintf(logbuf, sizeof(logbuf),
           "%s A%u R%u HR %.1fkH/s d%u",
           g_status.startsWith("share GOOD") ? "good " :
           g_status.startsWith("share BAD")  ? "rej  " :
           g_any_connected ? "alive" : "dead ",
           (unsigned)acc, (unsigned)rej, total_kh, (unsigned)diff);
  out.logLine40 = String(logbuf);
}
