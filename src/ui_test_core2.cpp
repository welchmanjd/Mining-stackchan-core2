#include "ui_test_core2.h"
#include <M5Unified.h>

static const char* phaseStr_(TestPhase p) {
  switch (p) {
    case TestPhase::Idle:         return "IDLE";
    case TestPhase::RunningSingle:return "SINGLE";
    case TestPhase::RunningBatch: return "BATCH";
    default: return "?";
  }
}

void UITestCore2::begin() {
  // nothing heavy; main controls brightness
}

void UITestCore2::markDirty() {
  dirty_ = true;
}

void UITestCore2::draw(uint32_t nowMs, const TestState& st) {
  if (!dirty_ && (nowMs - lastDrawMs_) < 200) return;
  lastDrawMs_ = nowMs;

  auto& d = M5.Display;

  if (dirty_) {
    d.fillScreen(BLACK);
    dirty_ = false;
  } else {
    // 軽く上書き（全消ししない）
    d.fillRect(0, 0, 320, 240, BLACK);
  }

  d.setTextColor(WHITE, BLACK);
  d.setTextSize(2);
  d.setCursor(0, 0);
  d.print("TEST: Azure TTS");

  d.setTextSize(1);
  d.setCursor(0, 22);
  d.printf("KeepAlive:%s   ChunkTO:%us",
           st.cfg.keepAlive ? "ON " : "OFF",
           (unsigned)(st.cfg.chunkTotalTimeoutMs / 1000));

  d.setCursor(0, 34);
  d.printf("Playback:%s  (tap mid to toggle)",
           st.playbackEnabled ? "ON " : "OFF");

  d.setTextSize(2);
  d.setCursor(0, 52);
  d.printf("PHASE:%s", phaseStr_(st.phase));

  d.setTextSize(1);
  d.setCursor(0, 78);
  d.printf("Total:%u  OK:%u  Fail:%u  Reject:%u",
           (unsigned)st.total, (unsigned)st.ok, (unsigned)st.fail, (unsigned)st.reject);

  if (st.phase == TestPhase::RunningBatch) {
    d.setCursor(0, 92);
    d.printf("Batch: %u / %u", (unsigned)st.batchDone, (unsigned)st.batchTarget);
  }

  d.setCursor(0, 110);
  d.printf("Last: seq=%u ok=%d http=%d chunked=%d bytes=%u",
           (unsigned)st.last.seq,
           (int)st.last.ok,
           (int)st.last.httpCode,
           (int)st.last.chunked,
           (unsigned)st.last.bytes);

  d.setCursor(0, 122);
  d.printf("FetchMs: %u  Err: %s",
           (unsigned)st.last.fetchMs,
           st.last.err[0] ? st.last.err : "-");

  if (st.okForStats > 0) {
    uint32_t avg = (uint32_t)(st.sumFetchMs / st.okForStats);
    d.setCursor(0, 140);
    d.printf("Fetch(ms) min:%u avg:%u max:%u (ok=%u)",
             (unsigned)st.minFetchMs, (unsigned)avg, (unsigned)st.maxFetchMs,
             (unsigned)st.okForStats);
  }

  // simple touch buttons guide
  d.setCursor(0, 165);
  d.print("Tap topL:KA  topR:TO  mid:PLAY");

  d.fillRect(0, 190, 160, 50, 0x39E7);   // light gray
  d.fillRect(160, 190, 160, 50, 0x39E7);
  d.setTextColor(BLACK, 0x39E7);
  d.setTextSize(2);
  d.setCursor(20, 205);
  d.print("SINGLE");
  d.setCursor(190, 205);
  d.print("BATCH");
}



// src/main.cpp
// ===== Mining-chan Core2 — main entry (UI + orchestrator) =====
// Board   : M5Stack Core2
// Libs    : M5Unified, ArduinoJson, WiFi, WiFiClientSecure, HTTPClient, m5stack-avatar
// Notes   : マイニング処理は mining_task.* に分離。
//           画面描画は ui_mining_core2.h に集約。
