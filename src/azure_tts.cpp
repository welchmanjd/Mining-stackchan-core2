// src/azure_tts.cpp
// Azure TTS (SSML) -> WAV取得 -> M5Unifiedで再生
//
// 重要: Azure側が Transfer-Encoding: chunked を返すケースがあり、
// HTTPClient の生ストリームをそのまま read() するとチャンクサイズ行が混入して
// WAV先頭が "RIFF" ではなく "f52\r\nRIFF..." になり playWav が失敗する。
// この実装では chunked を検出してデチャンクしてからWAVとして扱う。
//
// 追加(2025-12):
// ・HTTP/TLSセッション(keep-alive)をできるだけ使い回してラグ短縮
// ・Wi-Fi切断を検知したら、次回に備えてセッションを捨てる

#include "azure_tts.h"

#include <M5Unified.h>
#include <WiFi.h>
#include <cstring>

#if defined(ESP32)
  #include <esp_heap_caps.h>
#endif

#include "logging.h"
#include "config.h"

// ---- config fallback (未定義でもコンパイルできるように) ----
#ifndef MC_AZ_TTS_ENDPOINT
  #define MC_AZ_TTS_ENDPOINT ""
#endif
#ifndef MC_AZ_SPEECH_REGION
  #define MC_AZ_SPEECH_REGION ""
#endif
#ifndef MC_AZ_SPEECH_KEY
  #define MC_AZ_SPEECH_KEY ""
#endif
#ifndef MC_AZ_TTS_VOICE
  #define MC_AZ_TTS_VOICE "ja-JP-AoiNeural"
#endif
#ifndef MC_AZ_TTS_OUTPUT_FORMAT
  // Azure TTS: WAV(RIFF) / 16kHz / 16bit / mono PCM
  #define MC_AZ_TTS_OUTPUT_FORMAT "riff-16khz-16bit-mono-pcm"
#endif
// keep-alive を無効化したいとき用（デバッグ）
#ifndef MC_AZ_TTS_KEEPALIVE
  #define MC_AZ_TTS_KEEPALIVE 1
#endif
// ----------------------------------------------------------

namespace {

static String buildEndpointFromRegion_(const char* region) {
  if (!region || !region[0]) return String("");
  String ep;
  ep.reserve(strlen(region) + 64);
  ep += "https://";
  ep += region;
  ep += ".tts.speech.microsoft.com/cognitiveservices/v1";
  return ep;
}

#if defined(ESP32)
static void logHeap_(const char* tag) {
  uint32_t freeInt = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  uint32_t maxBlk  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  mc_logf("[TTS] %s heap: free=%u int=%u maxblk=%u",
          tag, (unsigned)freeInt, (unsigned)freeInt, (unsigned)maxBlk);
}
#else
static void logHeap_(const char* tag) { (void)tag; }
#endif

static void logSpeaker_(const char* tag) {
  mc_logf("[TTS] %s spk: enabled=%d running=%d playing=%d vol=%d",
          tag,
          (int)M5.Speaker.isEnabled(),
          (int)M5.Speaker.isRunning(),
          (int)M5.Speaker.isPlaying(),
          (int)M5.Speaker.getVolume());
}

static void dumpHead16_(const uint8_t* p, size_t len) {
  if (!p || len < 16) return;
  mc_logf("[TTS] wav[0..15]=%02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X",
          p[0],p[1],p[2],p[3], p[4],p[5],p[6],p[7], p[8],p[9],p[10],p[11], p[12],p[13],p[14],p[15]);
}

// 先頭8バイト程度が「HEX\r\nRIFF」になっていたら剥がす（救済）
static bool salvageChunkedPrefix_(uint8_t** buf, size_t* len) {
  if (!buf || !*buf || !len || *len < 16) return false;
  uint8_t* p = *buf;
  size_t i = 0;
  while (i < 8 && i < *len) {
    char c = (char)p[i];
    bool hex = (c>='0'&&c<='9') || (c>='a'&&c<='f') || (c>='A'&&c<='F');
    if (!hex) break;
    i++;
  }
  if (i == 0 || i + 6 >= *len) return false;
  if (p[i] != '\r' || p[i+1] != '\n') return false;
  if (p[i+2] != 'R' || p[i+3] != 'I' || p[i+4] != 'F' || p[i+5] != 'F') return false;

  size_t drop = i + 2;
  memmove(p, p + drop, *len - drop);
  *len -= drop;
  mc_logf("[TTS] salvage: dropped %u bytes chunk header", (unsigned)drop);
  return true;
}

static bool readLine_(WiFiClient* s, char* out, size_t outSize, uint32_t timeoutMs) {
  if (!s || !out || outSize < 2) return false;
  size_t n = 0;
  uint32_t t0 = millis();
  while (true) {
    while (!s->available()) {
      if ((millis() - t0) > timeoutMs) return false;
      delay(1);
    }
    int c = s->read();
    if (c < 0) continue;
    if (c == '\n') { out[n] = '\0'; return true; }
    if (n + 1 < outSize) out[n++] = (char)c;
  }
}

static constexpr uint32_t kKeepAliveIdleResetMs      = 8000;   // これ以上空いたら使い回さない
static constexpr uint32_t kKeepAliveCooldownMs       = 15000;  // 失敗したらこの間は使い回し禁止
static constexpr uint32_t kBodyStartTimeoutReuseMs   = 2500;   // 使い回し時：ボディが来なければ即捨てる
static constexpr uint32_t kBodyStartTimeoutColdMs    = 8000;   // 新規接続時：少し長めに待つ
static constexpr uint32_t kChunkTotalTimeoutReuseMs  = 6000;   // 使い回し時：全体も短め
static constexpr uint32_t kChunkTotalTimeoutColdMs   = 20000;  // 新規接続時：全体は長め


static bool waitBodyStart_(WiFiClient* s, uint32_t timeoutMs) {
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    if (!s->connected()) return false;   // ← WiFiClient ならOK
    if (s->available() > 0) return true;
    delay(1);
  }
  return false;
}





// Transfer-Encoding: chunked をデコードして payload だけを outBuf に貯める
// --- [REPLACE] readChunked_ (parameterized timeouts) ---
static bool readChunked_(
    WiFiClient* s,
    uint8_t** outBuf,
    size_t* outLen,
    uint32_t totalTimeoutMs,
    uint32_t sizeLineTimeoutMs,
    uint32_t dataIdleTimeoutMs) {

  *outBuf = nullptr;
  *outLen = 0;
  if (!s) return false;

  const uint32_t tStart = millis();
  auto expired = [&]() -> bool {
    return (uint32_t)(millis() - tStart) > totalTimeoutMs;
  };

  size_t cap = 0, used = 0;
  uint8_t* buf = nullptr;
  bool completed = false;

  auto ensureCap = [&](size_t need) -> bool {
    if (need <= cap) return true;
    size_t newCap = (cap == 0) ? 4096 : cap * 2;
    while (newCap < need) newCap *= 2;
#if defined(ESP32)
    void* nb = heap_caps_realloc(buf, newCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!nb) nb = heap_caps_realloc(buf, newCap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    void* nb = realloc(buf, newCap);
#endif
    if (!nb) return false;
    buf = (uint8_t*)nb;
    cap = newCap;
    return true;
  };

  char line[64];

  for (;;) {
    if (expired()) { mc_logf("[TTS] chunked: total timeout"); goto fail; }

    // chunk-size line
    if (!readLine_(s, line, sizeof(line), sizeLineTimeoutMs)) {
      mc_logf("[TTS] chunked: size line timeout");
      goto fail;
    }

    size_t L = strlen(line);
    if (L && line[L-1] == '\r') line[L-1] = '\0';
    if (line[0] == '\0') continue;

    char* semi = strchr(line, ';');
    if (semi) *semi = '\0';

    unsigned long chunk = strtoul(line, nullptr, 16);

    if (chunk == 0) {
      // trailer headers (optional)
      for (;;) {
        if (expired()) break;
        if (!readLine_(s, line, sizeof(line), 2000)) break;
        size_t TL = strlen(line);
        if (TL && line[TL-1] == '\r') line[TL-1] = '\0';
        if (line[0] == '\0') break;
      }
      completed = true;
      break;
    }

    if (!ensureCap(used + (size_t)chunk)) {
      mc_logf("[TTS] chunked: realloc failed need=%u", (unsigned)(used + (size_t)chunk));
      goto fail;
    }

    size_t remain = (size_t)chunk;
    uint32_t lastDataMs = millis();

    while (remain > 0) {
      if (expired()) { mc_logf("[TTS] chunked: total timeout (data)"); goto fail; }

      int avail = s->available();
      if (avail <= 0) {
        if ((uint32_t)(millis() - lastDataMs) > dataIdleTimeoutMs) {
          mc_logf("[TTS] chunked: data idle timeout remain=%u", (unsigned)remain);
          goto fail;
        }
        delay(1);
        continue;
      }

      size_t toRead = (size_t)avail;
      if (toRead > remain) toRead = remain;

      int r = s->read(buf + used, toRead);
      if (r > 0) {
        used += (size_t)r;
        remain -= (size_t)r;
        lastDataMs = millis();
      } else {
        delay(1);
      }
    }

    // discard CRLF
    uint32_t t0 = millis();
    while (s->available() < 2) {
      if (expired() || (uint32_t)(millis() - t0) > 2000) break;
      delay(1);
    }
    if (s->available() >= 2) {
      (void)s->read();
      (void)s->read();
    }
  }

  if (!completed || used == 0) {
    mc_logf("[TTS] chunked: incomplete (used=%u)", (unsigned)used);
    goto fail;
  }

  *outBuf = buf;
  *outLen = used;
  return true;

fail:
  if (buf) free(buf);
  return false;
}



struct WavPcmInfo {
  const uint8_t* data = nullptr;
  uint32_t dataBytes = 0;
  uint32_t sampleRate = 0;
  uint16_t bitsPerSample = 0;
  uint16_t channels = 0;
  uint16_t audioFormat = 0;
};

static uint32_t rd32le_(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16le_(const uint8_t* p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

// PCM(16bit) monoのみを対象にdataチャンクを見つける
static bool parseWavPcm16Mono_(const uint8_t* wav, size_t len, WavPcmInfo* out) {
  if (!wav || len < 44 || !out) return false;
  if (memcmp(wav, "RIFF", 4) != 0) return false;
  if (memcmp(wav + 8, "WAVE", 4) != 0) return false;

  size_t pos = 12;
  bool haveFmt = false;

  WavPcmInfo info;

  while (pos + 8 <= len) {
    const uint8_t* ck = wav + pos;
    uint32_t ckSize = rd32le_(ck + 4);
    pos += 8;
    if (pos + ckSize > len) break;

    if (memcmp(ck, "fmt ", 4) == 0 && ckSize >= 16) {
      info.audioFormat   = rd16le_(wav + pos + 0);
      info.channels      = rd16le_(wav + pos + 2);
      info.sampleRate    = rd32le_(wav + pos + 4);
      info.bitsPerSample = rd16le_(wav + pos + 14);
      haveFmt = true;
    } else if (memcmp(ck, "data", 4) == 0) {
      info.data = wav + pos;
      info.dataBytes = ckSize;
      break;
    }

    pos += ckSize + (ckSize & 1);
  }

  if (!haveFmt || !info.data || info.dataBytes == 0) return false;
  if (info.audioFormat != 1) return false;         // PCM
  if (info.channels != 1) return false;            // mono
  if (info.bitsPerSample != 16) return false;      // 16-bit

  *out = info;
  return true;
}

static bool isHexByteLead_(int c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

} // namespace

// ---------------- AzureTts class ----------------

// --- [REPLACE] AzureTts::begin ---
void AzureTts::begin(uint8_t volume) {
  (void)volume;  // マスター音量は触らない

  endpoint_     = buildEndpointFromRegion_(MC_AZ_SPEECH_REGION);
  key_          = String(MC_AZ_SPEECH_KEY);
  defaultVoice_ = String(MC_AZ_TTS_VOICE);

  // ★ 昨日の方針：TLS/HTTPの再利用はしない
  keepaliveEnabled_ = false;

  // ★ 昨日の推奨タイムアウト（WAVでも“gap”系は有効）
  cfg_.keepAlive               = false;     // app_test_mode互換のため残すが、実装では常にOFF
  cfg_.httpTimeoutMs           = 2000;      // POST全体の基準
  cfg_.bodyStartTimeoutMs      = 1500;      // first byte
  cfg_.chunkTotalTimeoutMs     = 20000;     // WAVは大きいので少し余裕（必要なら後で詰めよう）
  cfg_.chunkSizeLineTimeoutMs  = 1500;
  cfg_.chunkDataIdleTimeoutMs  = 550;       // chunk gap
  cfg_.contentReadIdleTimeoutMs= 550;       // Content-Length時のgapも同じ扱い

  playbackEnabled_ = true;

  // token/dns/rate-limit state
  dnsWarmed_ = false;
  token_ = "";
  tokenExpireMs_ = 0;
  preferOldSts_ = true;
  lastRequestMs_ = 0;

  // セッションは保持しないが、互換のため初期化だけ
  client_.setInsecure();
}


bool AzureTts::isBusy() const {
  return state_ == Fetching || state_ == Ready || state_ == Playing;
}

void AzureTts::setRuntimeConfig(const RuntimeConfig& cfg) {
  cfg_ = cfg;
  keepaliveEnabled_ = cfg_.keepAlive;
}

AzureTts::RuntimeConfig AzureTts::runtimeConfig() const {
  return cfg_;
}

void AzureTts::setPlaybackEnabled(bool en) {
  playbackEnabled_ = en;
}

bool AzureTts::playbackEnabled() const {
  return playbackEnabled_;
}

AzureTts::LastResult AzureTts::lastResult() const {
  return last_;
}





void AzureTts::requestSessionReset() {
  sessionResetPending_ = true;

  // すでにIdleならこの場で捨ててOK（fetchタスクと競合しない）
  if (state_ == Idle && task_ == nullptr) {
    resetSession_();
  }
}

void AzureTts::resetSession_() {
  // HTTPClient::end() は reuse=true だと stop() しない場合があるので明示 stop
  https_.end();
  client_.stop();
  sessionResetPending_ = false;
  mc_logf("[TTS] session reset");
}

bool AzureTts::speakAsync(const String& text, const char* voice) {
  if (isBusy()) return false;

  // start new seq
  last_ = LastResult();
  last_.seq = ++seq_;
  last_.keepAlive = keepaliveEnabled_;

  // safe point: handle pending reset
  if (sessionResetPending_) {
    resetSession_();
  }

  if (WiFi.status() != WL_CONNECTED) {
    mc_logf("[TTS] WiFi not connected");
    return false;
  }

  if (endpoint_.length() == 0 || key_.length() == 0) {
    mc_logf("[TTS] Azure config missing (endpoint/key)");
    return false;
  }

  reqText_  = text;
  reqVoice_ = (voice && voice[0]) ? String(voice) : defaultVoice_;

  state_ = Fetching;

  BaseType_t ok = xTaskCreatePinnedToCore(
      AzureTts::taskEntry,
      "azure_tts",
      12 * 1024,
      this,
      3,
      &task_,
      0
  );

  if (ok != pdPASS) {
    mc_logf("[TTS] task create failed");
    task_ = nullptr;
    state_ = Idle;
    return false;
  }
  return true;
}

void AzureTts::poll() {
  // safe point: handle pending reset
  if (state_ == Idle && sessionResetPending_ && task_ == nullptr) {
    resetSession_();
  }

  if (state_ == Error) {
    if (wav_) {
      free(wav_);
      wav_ = nullptr;
      wavLen_ = 0;
    }
    state_ = Idle;
    return;
  }

  if (state_ == Ready) {
    if (!wav_ || wavLen_ == 0) {
      mc_logf("[TTS] Ready but wav empty");
      state_ = Error;
      return;
    }

    logHeap_("pre-play");
    logSpeaker_("pre-play");
    dumpHead16_(wav_, wavLen_);

    if (!playbackEnabled_) {
      // fetchだけ成功したらOKとして捌く（テスト用）
      if (wav_) free(wav_);
      wav_ = nullptr;
      wavLen_ = 0;
      state_ = Idle;
      return;
    }

    bool playOk = M5.Speaker.playWav(wav_, wavLen_, 1, 7, true);
    if (!playOk) {
      mc_logf("[TTS] playWav failed -> trying playRaw fallback");

      WavPcmInfo info;
      if (!parseWavPcm16Mono_(wav_, wavLen_, &info)) {
        mc_logf("[TTS] WAV parse failed (not RIFF/WAVE or unsupported fmt)");
        state_ = Error;
        return;
      }

      const int16_t* pcm = (const int16_t*)info.data;
      size_t samples = (size_t)(info.dataBytes / 2);

      bool rawOk = M5.Speaker.playRaw(pcm, samples, info.sampleRate, false, 7, true);
      if (!rawOk) {
        mc_logf("[TTS] playRaw failed");
        state_ = Error;
        return;
      }
      mc_logf("[TTS] playing (raw)...");
      state_ = Playing;
      return;
    }

    mc_logf("[TTS] playing (wav)...");
    state_ = Playing;
    return;
  }

  if (state_ == Playing) {
    if (!M5.Speaker.isPlaying()) {
      mc_logf("[TTS] done");
      if (wav_) free(wav_);
      wav_ = nullptr;
      wavLen_ = 0;
      state_ = Idle;
    }
    return;
  }
}


void AzureTts::taskEntry(void* pv) {
  AzureTts* self = static_cast<AzureTts*>(pv);
  if (self) {
    self->taskBody();
  }
  vTaskDelete(nullptr);
}


// --- [ADD] AzureTts::warmupDnsOnce_ ---
void AzureTts::warmupDnsOnce_() {
  if (dnsWarmed_) return;
  dnsWarmed_ = true;

  if (!MC_AZ_SPEECH_REGION || !MC_AZ_SPEECH_REGION[0]) return;

  IPAddress ip;
  String host = String(MC_AZ_SPEECH_REGION) + ".tts.speech.microsoft.com";
  (void)WiFi.hostByName(host.c_str(), ip);  // DNS warm-up
}

// --- [ADD] AzureTts::fetchTokenOld_ ---
bool AzureTts::fetchTokenOld_(String* outTok) {
  if (!outTok) return false;
  outTok->clear();

  if (key_.length() == 0 || !MC_AZ_SPEECH_REGION[0]) return false;

  const String url = String("https://") + MC_AZ_SPEECH_REGION +
                     ".api.cognitive.microsoft.com/sts/v1.0/issueToken";

  WiFiClientSecure c;
  c.setInsecure();

  HTTPClient h;
  h.setReuse(false);
  h.useHTTP10(true);
  h.setTimeout(2000);

  if (!h.begin(c, url)) {
    h.end();
    return false;
  }

  h.addHeader("Ocp-Apim-Subscription-Key", key_);
  int code = h.POST((uint8_t*)nullptr, 0);

  if (code == 200) {
    String tok = h.getString();
    tok.trim();
    if (tok.length()) {
      *outTok = tok;
      h.end();
      return true;
    }
  }

  h.end();
  return false;
}

// --- [ADD] AzureTts::fetchTokenNew_ ---
bool AzureTts::fetchTokenNew_(String* outTok) {
  if (!outTok) return false;
  outTok->clear();

  if (key_.length() == 0 || !MC_AZ_SPEECH_REGION[0]) return false;

  const String url = String("https://") + MC_AZ_SPEECH_REGION +
                     ".sts.speech.microsoft.com/cognitiveservices/api/v1/token";

  WiFiClientSecure c;
  c.setInsecure();

  HTTPClient h;
  h.setReuse(false);
  h.useHTTP10(true);
  h.setTimeout(2000);

  if (!h.begin(c, url)) {
    h.end();
    return false;
  }

  h.addHeader("Ocp-Apim-Subscription-Key", key_);
  int code = h.POST((uint8_t*)nullptr, 0);

  if (code == 200) {
    String tok = h.getString();
    tok.trim();
    if (tok.length()) {
      *outTok = tok;
      h.end();
      return true;
    }
  }

  h.end();
  return false;
}

// --- [ADD] AzureTts::ensureToken_ ---
bool AzureTts::ensureToken_() {
  const uint32_t now = millis();

  // 1分余裕を見て有効判定
  if (token_.length() && (int32_t)(tokenExpireMs_ - (now + 60000UL)) > 0) {
    return true;
  }

  String tok;
  bool ok = false;

  if (preferOldSts_) {
    if (fetchTokenOld_(&tok)) { ok = true; preferOldSts_ = true; }
    else if (fetchTokenNew_(&tok)) { ok = true; preferOldSts_ = false; }
  } else {
    if (fetchTokenNew_(&tok)) { ok = true; preferOldSts_ = false; }
    else if (fetchTokenOld_(&tok)) { ok = true; preferOldSts_ = true; }
  }

  if (ok) {
    token_ = tok;
    tokenExpireMs_ = now + 9UL * 60UL * 1000UL; // 9分キャッシュ
    return true;
  }

  token_.clear();
  tokenExpireMs_ = 0;
  return false;
}


// --- [REPLACE] AzureTts::taskBody ---
void AzureTts::taskBody() {
  const String text  = reqText_;
  const String voice = reqVoice_;
  const String ssml  = buildSsml_(text, voice);

  mc_logf("[TTS] fetch start (len=%d)", (int)text.length());

  uint8_t* buf = nullptr;
  size_t   len = 0;

  const uint32_t tAll0 = millis();

  auto makeIntervalMs = [&]() -> uint32_t {
    // 1.2–1.5s + ±200ms
    int32_t base = 1200 + (int32_t)random(0, 301);     // 1200..1500
    int32_t jit  = (int32_t)random(-200, 201);         // -200..+200
    int32_t v = base + jit;
    if (v < 600) v = 600;
    return (uint32_t)v;
  };

  auto enforceInterval = [&](uint32_t intervalMs) {
    if (lastRequestMs_ == 0) return;
    const uint32_t now = millis();
    const uint32_t due = lastRequestMs_ + intervalMs;
    const int32_t wait = (int32_t)(due - now);
    if (wait > 0) delay((uint32_t)wait);
  };

  bool ok = false;

  for (int attempt = 0; attempt < 2; ++attempt) {
    const uint32_t intervalMs = makeIntervalMs();
    enforceInterval(intervalMs);

    lastRequestMs_ = millis();

    if (buf) { free(buf); buf = nullptr; }
    len = 0;

    ok = fetchWav_(ssml, &buf, &len);
    if (ok) break;

    if (attempt == 0) {
      // 指数バックオフ（1回だけ）
      const uint32_t backoff = 250 + (uint32_t)random(0, 251); // 250..500ms
      delay(backoff);
      continue;
    }
  }

  last_.fetchMs = millis() - tAll0;

  if (!ok) {
    mc_logf("[TTS] fetch failed");
    if (buf) free(buf);
    buf = nullptr;
    len = 0;

    if (last_.err[0] == '\0') strncpy(last_.err, "fetch", sizeof(last_.err)-1);
    last_.ok = false;

    state_ = Error;
    task_ = nullptr;
    return;
  }

  (void)salvageChunkedPrefix_(&buf, &len);

  mc_logf("[TTS] fetch ok: %u bytes", (unsigned)len);
  dumpHead16_(buf, len);

  last_.ok = true;
  last_.bytes = (uint32_t)len;

  wav_    = buf;
  wavLen_ = len;
  state_  = Ready;

  task_ = nullptr;
}

String AzureTts::xmlEscape_(const String& s) {
  // SSML/XMLで壊れやすい文字だけをエスケープする
  // UTF-8日本語はそのままでOK（& < > " ' だけ置換）
  String out;
  out.reserve(s.length() + 16);

  for (size_t i = 0; i < s.length(); ++i) {
    const char c = s[i];
    switch (c) {
      case '&':  out += F("&amp;");  break;
      case '<':  out += F("&lt;");   break;
      case '>':  out += F("&gt;");   break;
      case '"':  out += F("&quot;"); break;
      case '\'': out += F("&apos;"); break;
      default:   out += c;           break;
    }
  }
  return out;
}



String AzureTts::buildSsml_(const String& text, const String& voice) const {
  String t = xmlEscape_(text);
  String v = xmlEscape_(voice);

  String ssml;
  ssml.reserve(t.length() + v.length() + 120);
  ssml += F("<speak version='1.0' xml:lang='ja-JP' xmlns='http://www.w3.org/2001/10/synthesis'>");
  ssml += F("<voice name='");
  ssml += v;
  ssml += F("'>");
  ssml += t;
  ssml += F("</voice></speak>");
  return ssml;
}

// --- [REPLACE] AzureTts::fetchWav_ ---
bool AzureTts::fetchWav_(const String& ssml, uint8_t** outBuf, size_t* outLen) {
  *outBuf = nullptr;
  *outLen = 0;

  if (WiFi.status() != WL_CONNECTED) {
    mc_logf("[TTS] fetch: WiFi not connected");
    strncpy(last_.err, "wifi", sizeof(last_.err)-1);
    return false;
  }

  warmupDnsOnce_();

  // Token（失敗してもサブスクキーにフォールバック）
  const bool tokenOk = ensureToken_();
  if (!tokenOk) {
    mc_logf("[TTS] token failed -> fallback to key header");
  }

  WiFiClientSecure cli;
  cli.setInsecure();

  HTTPClient http;
  http.setReuse(false);      // ★再利用しない
  http.useHTTP10(true);      // ★chunked回避を狙う（ただし念のためchunk decodeは残す）
  http.setTimeout(cfg_.httpTimeoutMs);

  if (!http.begin(cli, endpoint_)) {
    mc_logf("[TTS] http.begin failed");
    strncpy(last_.err, "begin", sizeof(last_.err)-1);
    http.end();
    return false;
  }

  const char* hdrKeys[] = {"Content-Type","Transfer-Encoding","Content-Length","Connection"};
  http.collectHeaders(hdrKeys, 4);

  http.addHeader("Content-Type", "application/ssml+xml");
  http.addHeader("X-Microsoft-OutputFormat", MC_AZ_TTS_OUTPUT_FORMAT);
  http.addHeader("User-Agent", "Mining-Stackchan-Core2");
  http.addHeader("Accept", "audio/wav");
  http.addHeader("Accept-Encoding", "identity");
  http.addHeader("Connection", "close");

  if (tokenOk) {
    http.addHeader("Authorization", String("Bearer ") + token_);
  } else {
    http.addHeader("Ocp-Apim-Subscription-Key", key_);
  }

  last_.keepAlive = false;

  int httpCode = http.POST((uint8_t*)ssml.c_str(), ssml.length());
  last_.httpCode = httpCode;

  if (httpCode != 200) {
    mc_logf("[TTS] HTTP %d", httpCode);
    strncpy(last_.err, "http", sizeof(last_.err)-1);

    String body = http.getString();
    if (body.length()) {
      body = body.substring(0, 200);
      mc_logf("[TTS] err body: %s", body.c_str());
    }

    http.end();
    return false;
  }

  const String te = http.header("Transfer-Encoding");
  const String cl = http.header("Content-Length");
  mc_logf("[TTS] resp hdr: te=%s cl=%s", te.c_str(), cl.c_str());

  WiFiClient* stream = http.getStreamPtr();
  if (!waitBodyStart_(stream, cfg_.bodyStartTimeoutMs)) {
    mc_logf("[TTS] body start timeout");
    strncpy(last_.err, "firstbyte", sizeof(last_.err)-1);
    http.end();
    return false;
  }

  int total = http.getSize();  // Content-Length（不明なら -1）
  const bool hdrChunked = (te.indexOf("chunked") >= 0);

  // ヘッダが取れない時の保険：先頭がHEXなら chunked かも
  bool maybeChunked = hdrChunked;
  if (!maybeChunked) {
    int p = stream->peek();
    if (p >= 0 && isHexByteLead_(p)) maybeChunked = true;
  }
  last_.chunked = maybeChunked;

  // 1) Content-Length が取れている
  if (total > 0 && !maybeChunked) {
    uint8_t* buf = nullptr;
#if defined(ESP32)
    buf = (uint8_t*)heap_caps_malloc((size_t)total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = (uint8_t*)heap_caps_malloc((size_t)total, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    buf = (uint8_t*)malloc((size_t)total);
#endif
    if (!buf) {
      mc_logf("[TTS] malloc failed (%d)", total);
      strncpy(last_.err, "malloc", sizeof(last_.err)-1);
      http.end();
      return false;
    }

    size_t readTotal = 0;
    const uint32_t tStart = millis();
    uint32_t lastDataMs = millis();

    while (readTotal < (size_t)total) {
      if ((uint32_t)(millis() - tStart) > cfg_.chunkTotalTimeoutMs) break;

      int avail = stream->available();
      if (avail > 0) {
        size_t toRead = (size_t)avail;
        size_t remain = (size_t)total - readTotal;
        if (toRead > remain) toRead = remain;

        int r = stream->read(buf + readTotal, toRead);
        if (r > 0) {
          readTotal += (size_t)r;
          lastDataMs = millis();
        } else {
          delay(1);
        }
      } else {
        if ((uint32_t)(millis() - lastDataMs) > cfg_.contentReadIdleTimeoutMs) break;
        delay(1);
      }
    }

    http.end();

    if (readTotal != (size_t)total) {
      mc_logf("[TTS] read short %u/%u", (unsigned)readTotal, (unsigned)total);
      strncpy(last_.err, "short", sizeof(last_.err)-1);
      free(buf);
      return false;
    }

    *outBuf = buf;
    *outLen = readTotal;
    return true;
  }

  // 2) chunked（または怪しい）
  if (maybeChunked) {
    uint8_t* buf = nullptr;
    size_t len = 0;
    bool ok = readChunked_(stream, &buf, &len,
                          cfg_.chunkTotalTimeoutMs,
                          cfg_.chunkSizeLineTimeoutMs,
                          cfg_.chunkDataIdleTimeoutMs);
    http.end();

    if (!ok || len == 0) {
      mc_logf("[TTS] chunked decode failed");
      strncpy(last_.err, "chunk", sizeof(last_.err)-1);
      if (buf) free(buf);
      return false;
    }

    *outBuf = buf;
    *outLen = len;
    return true;
  }

  // 3) それ以外（長さ不明・非chunked）フォールバック：closeまで読む
  size_t cap = 0, used = 0;
  uint8_t* buf = nullptr;
  uint8_t tmp[1024];

  const uint32_t tStart = millis();
  uint32_t lastDataMs = millis();

  auto ensureCap = [&](size_t need) -> bool {
    if (need <= cap) return true;
    size_t newCap = (cap == 0) ? 4096 : cap * 2;
    while (newCap < need) newCap *= 2;
#if defined(ESP32)
    void* nb = heap_caps_realloc(buf, newCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!nb) nb = heap_caps_realloc(buf, newCap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    void* nb = realloc(buf, newCap);
#endif
    if (!nb) return false;
    buf = (uint8_t*)nb;
    cap = newCap;
    return true;
  };

  while (http.connected() || stream->available()) {
    if ((uint32_t)(millis() - tStart) > cfg_.chunkTotalTimeoutMs) break;

    int avail = stream->available();
    if (avail <= 0) {
      if ((uint32_t)(millis() - lastDataMs) > cfg_.contentReadIdleTimeoutMs) break;
      delay(1);
      continue;
    }

    size_t toRead = (size_t)avail;
    if (toRead > sizeof(tmp)) toRead = sizeof(tmp);

    int r = stream->read(tmp, toRead);
    if (r <= 0) { delay(1); continue; }

    if (!ensureCap(used + (size_t)r)) {
      mc_logf("[TTS] realloc failed");
      strncpy(last_.err, "realloc", sizeof(last_.err)-1);
      if (buf) free(buf);
      http.end();
      return false;
    }

    memcpy(buf + used, tmp, (size_t)r);
    used += (size_t)r;
    lastDataMs = millis();
  }

  http.end();

  if (used == 0) {
    mc_logf("[TTS] empty response");
    strncpy(last_.err, "empty", sizeof(last_.err)-1);
    if (buf) free(buf);
    return false;
  }

  *outBuf = buf;
  *outLen = used;
  return true;
}
