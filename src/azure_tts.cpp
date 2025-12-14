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
static bool readChunked_(WiFiClient* s, uint8_t** outBuf, size_t* outLen) {
  *outBuf = nullptr;
  *outLen = 0;

  const uint32_t kTotalTimeoutMs = 15000;   // 全体で15秒で諦める
  const uint32_t tStart = millis();

  auto expired = [&]() -> bool {
    return (millis() - tStart) > kTotalTimeoutMs;
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

    // ★サイズ行は短めに待つ（ここで詰まると長時間停止するので）
    if (!readLine_(s, line, sizeof(line), 3000)) {
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
      // trailer headers（あってもなくてもOK）
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
    uint32_t t0 = millis();

    while (remain > 0) {
      if (expired()) { mc_logf("[TTS] chunked: total timeout (data)"); goto fail; }

      int avail = s->available();
      if (avail <= 0) {
        if ((millis() - t0) > 5000) { // ★データも5秒止まったら失敗
          mc_logf("[TTS] chunked: data timeout remain=%u", (unsigned)remain);
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
        t0 = millis();
      } else {
        delay(1);
      }
    }

    // discard CRLF
    uint32_t td = millis();
    while (s->available() < 2) {
      if (expired() || (millis() - td) > 2000) break;
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

void AzureTts::begin(uint8_t volume) {
  (void)volume;  // マスター音量は触らない（タッチ音まで変わるため）

  endpoint_     = buildEndpointFromRegion_(MC_AZ_SPEECH_REGION);
  key_          = String(MC_AZ_SPEECH_KEY);
  defaultVoice_ = String(MC_AZ_TTS_VOICE);

  // keep-alive session init
  keepaliveEnabled_ = (MC_AZ_TTS_KEEPALIVE != 0);

  cfg_.keepAlive = keepaliveEnabled_;
  cfg_.httpTimeoutMs = 20000;
  cfg_.bodyStartTimeoutMs = 900;
  cfg_.chunkTotalTimeoutMs = 15000;
  cfg_.chunkSizeLineTimeoutMs = 3000;
  cfg_.chunkDataIdleTimeoutMs = 5000;
  cfg_.contentReadIdleTimeoutMs = 20000;

  playbackEnabled_ = true;

  client_.setInsecure();
  // NOTE: setTimeout() is not available on all cores; HTTPClient timeout is used.
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
  static_cast<AzureTts*>(pv)->taskBody();
  vTaskDelete(nullptr);
}

void AzureTts::taskBody() {
  const String text  = reqText_;
  const String voice = reqVoice_;
  String ssml = buildSsml_(text, voice);

  uint8_t* buf = nullptr;
  size_t   len = 0;

  mc_logf("[TTS] fetch start (len=%d)", (int)text.length());

  uint32_t t0 = millis();
  bool ok = fetchWav_(ssml, &buf, &len);
  last_.fetchMs = millis() - t0;
  if (!ok) {
    mc_logf("[TTS] fetch failed");
    if (buf) free(buf);
    state_ = Error;
    task_ = nullptr;

    if (last_.err[0] == '\0') strncpy(last_.err, "fetch_failed", sizeof(last_.err)-1);
    last_.ok = false;

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
      default:   out += c; break;
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

bool AzureTts::fetchWav_(const String& ssml, uint8_t** outBuf, size_t* outLen) {
  *outBuf = nullptr;
  *outLen = 0;

  if (WiFi.status() != WL_CONNECTED) {
    mc_logf("[TTS] fetch: WiFi not connected");
    return false;
  }

  // If Wi-Fi got reconnected etc., reset keep-alive session first.
  if (sessionResetPending_) {
    resetSession_();
  }

  https_.setTimeout(20000);
  https_.setReuse(keepaliveEnabled_);
  https_.useHTTP10(!keepaliveEnabled_);  // keep-aliveしたい時はHTTP/1.1

  const char* endpoint = endpoint_.c_str();

  // 失敗時のリトライは1回だけ（keep-aliveが切れてた時に効く）
  for (int attempt = 0; attempt < 2; ++attempt) {

    if (!https_.begin(client_, endpoint)) {
      mc_logf("[TTS] https.begin failed (attempt=%d)", attempt);
      resetSession_();
      continue;
    }

    const char* hdrKeys[] = {"Content-Type","Content-Encoding","Transfer-Encoding","Content-Length","Connection"};
    https_.collectHeaders(hdrKeys, 5);

    https_.addHeader("Ocp-Apim-Subscription-Key", key_.c_str());
    https_.addHeader("Content-Type", "application/ssml+xml");
    https_.addHeader("X-Microsoft-OutputFormat", MC_AZ_TTS_OUTPUT_FORMAT);
    https_.addHeader("User-Agent", "Mining-Stackchan-Core2");
    https_.addHeader("Accept", "audio/wav");
    https_.addHeader("Accept-Encoding", "identity");
    https_.addHeader("Connection", keepaliveEnabled_ ? "keep-alive" : "close");

    int httpCode = https_.POST((uint8_t*)ssml.c_str(), ssml.length());
    if (httpCode != 200) {
      mc_logf("[TTS] HTTP %d (attempt=%d)", httpCode, attempt);
      String body = https_.getString();
      if (body.length()) {
        String head = body.substring(0, 240);
        mc_logf("[TTS] body: %s", head.c_str());
      }
      https_.end();

      last_.httpCode = httpCode;
      strncpy(last_.err, "http", sizeof(last_.err)-1);

      // keep-aliveが壊れてる/切れてる時は 1回だけ作り直して再試行
      if (attempt == 0) {
        resetSession_();
        continue;
      }
      return false;
    }

    String ct = https_.header("Content-Type");
    String ce = https_.header("Content-Encoding");
    String te = https_.header("Transfer-Encoding");
    String cl = https_.header("Content-Length");
    String cn = https_.header("Connection");

    mc_logf("[TTS] resp hdr: type=%s enc=%s te=%s cl=%s conn=%s",
            ct.c_str(), ce.c_str(), te.c_str(), cl.c_str(), cn.c_str());

    int total = https_.getSize();  // Content-Length（不明なら -1）
    mc_logf("[TTS] resp size=%d", total);

    logHeap_("pre-alloc");
    logSpeaker_("pre-alloc");

    WiFiClient* stream = https_.getStreamPtr();

    // keep-alive の「死んだ接続」対策：本文が来なければすぐ捨ててリトライ
    uint32_t tWait = millis();
    while (stream->available() == 0 && https_.connected() && (millis() - tWait) < 900) {
    delay(1);
    }
    if (stream->available() == 0) {
    mc_logf("[TTS] body not arrived -> stale keep-alive, reset+retry");
    https_.end();
    resetSession_();     // ← これが効く
    continue;            // 次の attempt へ
    }

    // 1) Content-Length が分かる場合
    if (total > 0) {
      uint8_t* buf = nullptr;
#if defined(ESP32)
      buf = (uint8_t*)heap_caps_malloc((size_t)total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!buf) buf = (uint8_t*)heap_caps_malloc((size_t)total, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
      buf = (uint8_t*)malloc((size_t)total);
#endif
      if (!buf) {
        mc_logf("[TTS] malloc failed (%d)", total);
        https_.end();
        return false;
      }

      size_t readTotal = 0;
      uint32_t t0 = millis();

      while (readTotal < (size_t)total) {
        int avail = stream->available();
        if (avail > 0) {
          size_t toRead = (size_t)avail;
          size_t remain = (size_t)total - readTotal;
          if (toRead > remain) toRead = remain;

          int r = stream->read((uint8_t*)buf + readTotal, toRead);
          if (r > 0) {
            readTotal += (size_t)r;
            t0 = millis();
          } else {
            delay(1);
          }
        } else {
          if ((millis() - t0) > 20000) break;
          delay(1);
        }
      }

      https_.end();

      if (readTotal != (size_t)total) {
        mc_logf("[TTS] read short %u/%u", (unsigned)readTotal, (unsigned)total);
        free(buf);
        return false;
      }

      *outBuf = buf;
      *outLen = readTotal;
      return true;
    }

    // 2) Content-Length 不明: chunked を優先してデコード
    bool maybeChunked = (te.indexOf("chunked") >= 0);

    last_.chunked = maybeChunked;

    // ヘッダが取れなかった場合でも、先頭1byteがHEXなら chunked の可能性が高い
    if (!maybeChunked) {
      int p = stream->peek();
      if (p >= 0 && isHexByteLead_(p)) {
        maybeChunked = true;
      }
    }

    if (maybeChunked) {
      uint8_t* buf = nullptr;
      size_t len = 0;
      bool ok2 = readChunked_(stream, &buf, &len);
      https_.end();
      if (!ok2) {
        mc_logf("[TTS] chunked decode failed");
        strncpy(last_.err, "chunk_timeout", sizeof(last_.err)-1);

        free(buf);
        // try again by resetting session once
        if (attempt == 0) {
          resetSession_();
          continue;
        }
        return false;
      }
      *outBuf = buf;
      *outLen = len;
      return true;
    }

    // 3) それ以外フォールバック（Connection: close の時など）
    size_t cap = 0, used = 0;
    uint8_t* buf = nullptr;
    uint8_t tmp[1024];
    uint32_t t0 = millis();

    while (https_.connected() || stream->available()) {
      int avail = stream->available();
      if (avail <= 0) {
        if ((millis() - t0) > 20000) break;
        delay(1);
        continue;
      }
      size_t toRead = (size_t)avail;
      if (toRead > sizeof(tmp)) toRead = sizeof(tmp);

      int r = stream->read(tmp, toRead);
      if (r <= 0) { delay(1); continue; }

      if (used + (size_t)r > cap) {
        size_t newCap = (cap == 0) ? 4096 : cap * 2;
        while (newCap < used + (size_t)r) newCap *= 2;
#if defined(ESP32)
        void* nb = heap_caps_realloc(buf, newCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!nb) nb = heap_caps_realloc(buf, newCap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
        void* nb = realloc(buf, newCap);
#endif
        if (!nb) {
          mc_logf("[TTS] realloc failed (%u)", (unsigned)newCap);
          if (buf) free(buf);
          https_.end();
          return false;
        }
        buf = (uint8_t*)nb;
        cap = newCap;
      }

      memcpy(buf + used, tmp, (size_t)r);
      used += (size_t)r;
      t0 = millis();
    }

    https_.end();

    if (used == 0) {
      if (buf) free(buf);
      mc_logf("[TTS] empty response");
      // try again with reset once
      if (attempt == 0) {
        resetSession_();
        continue;
      }
      return false;
    }

    *outBuf = buf;
    *outLen = used;
    return true;
  }

  return false;
}
