#include "app_test_mode.h"
#include "logging.h"

void AppTestMode::begin(AzureTts* tts) {
  tts_ = tts;
}

void AppTestMode::enter(uint32_t nowMs) {
  mc_logf("[TEST] enter (t=%u)", (unsigned)nowMs);

  st_ = TestState(); // reset all

  if (tts_) {
    // テストは“速く回したい”ので、デフォルトは再生OFF（必要なら切替）
    tts_->setPlaybackEnabled(false);
    st_.playbackEnabled = false;

    st_.cfg = tts_->runtimeConfig();
    st_.last = tts_->lastResult();
  }

  waiting_ = false;
  startedAtMs_ = 0;
  nextStartMs_ = 0;
  activeSeq_ = 0;
}

void AppTestMode::exit(uint32_t nowMs) {
  mc_logf("[TEST] exit (t=%u)", (unsigned)nowMs);

  stopBatch_();

  // 通常モードに戻るので再生ONに戻す（好みで）
  if (tts_) {
    tts_->setPlaybackEnabled(true);
  }
}

bool AppTestMode::hit_(int x, int y, int x0, int y0, int w, int h) const {
  return (x >= x0 && x < (x0 + w) && y >= y0 && y < (y0 + h));
}

void AppTestMode::toggleKeepAlive_() {
  if (!tts_) return;
  auto cfg = tts_->runtimeConfig();
  cfg.keepAlive = !cfg.keepAlive;
  tts_->setRuntimeConfig(cfg);
  st_.cfg = cfg;
  mc_logf("[TEST] keepAlive=%d", (int)cfg.keepAlive);
}

void AppTestMode::toggleChunkTimeout_() {
  if (!tts_) return;
  auto cfg = tts_->runtimeConfig();
  cfg.chunkTotalTimeoutMs = (cfg.chunkTotalTimeoutMs <= 20000) ? 60000 : 15000;
  tts_->setRuntimeConfig(cfg);
  st_.cfg = cfg;
  mc_logf("[TEST] chunkTotalTimeoutMs=%u", (unsigned)cfg.chunkTotalTimeoutMs);
}

void AppTestMode::togglePlayback_() {
  if (!tts_) return;
  st_.playbackEnabled = !st_.playbackEnabled;
  tts_->setPlaybackEnabled(st_.playbackEnabled);
  mc_logf("[TEST] playback=%d", (int)st_.playbackEnabled);
}

void AppTestMode::stopBatch_() {
  if (st_.phase == TestPhase::RunningBatch) {
    mc_logf("[TEST] stop batch");
  }
  st_.phase = TestPhase::Idle;
  st_.batchDone = 0;
}

void AppTestMode::startOne_(uint32_t nowMs) {
  if (!tts_) return;

  if (tts_->isBusy()) {
    // “押したけどbusy”は reject 扱いにして見える化
    st_.reject++;
    st_.total++;
    st_.last = tts_->lastResult();
    strncpy(st_.last.err, "reject:busy", sizeof(st_.last.err) - 1);
    st_.last.err[sizeof(st_.last.err) - 1] = '\0';
    return;
  }

  // start
  bool ok = tts_->speakAsync(text_);
  if (!ok) {
    st_.reject++;
    st_.total++;
    st_.last = tts_->lastResult();
    strncpy(st_.last.err, "reject:speakAsync", sizeof(st_.last.err) - 1);
    st_.last.err[sizeof(st_.last.err) - 1] = '\0';
    return;
  }

  st_.total++;
  waiting_ = true;
  startedAtMs_ = nowMs;

  // “この開始に紐づくseq”を覚える
  activeSeq_ = tts_->lastResult().seq;

  mc_logf("[TEST] start seq=%u", (unsigned)activeSeq_);
}

void AppTestMode::onCompleted_() {
  if (!tts_) return;

  st_.last = tts_->lastResult();

  if (st_.last.ok) {
    st_.ok++;
    st_.okForStats++;
    st_.sumFetchMs += st_.last.fetchMs;

    if (st_.minFetchMs == 0 || st_.last.fetchMs < st_.minFetchMs) st_.minFetchMs = st_.last.fetchMs;
    if (st_.last.fetchMs > st_.maxFetchMs) st_.maxFetchMs = st_.last.fetchMs;
  } else {
    st_.fail++;
  }
}

void AppTestMode::update(uint32_t nowMs, const TestInput& in) {
  st_.lastUpdateMs = nowMs;

  // touch UI:
  //  - top-left  : keepAlive toggle
  //  - top-right : chunk timeout 15s <-> 60s
  //  - mid       : playback toggle
  //  - bottom L  : single
  //  - bottom R  : batch toggle
  if (in.touchDown) {
    if (hit_(in.touchX, in.touchY, 0, 0, 160, 50)) {
      toggleKeepAlive_();
    } else if (hit_(in.touchX, in.touchY, 160, 0, 160, 50)) {
      toggleChunkTimeout_();
    } else if (hit_(in.touchX, in.touchY, 0, 90, 320, 50)) {
      togglePlayback_();
    } else if (hit_(in.touchX, in.touchY, 0, 190, 160, 50)) {
      // single
      if (st_.phase == TestPhase::Idle) {
        st_.phase = TestPhase::RunningSingle;
        startOne_(nowMs);
      }
    } else if (hit_(in.touchX, in.touchY, 160, 190, 160, 50)) {
      // batch toggle
      if (st_.phase != TestPhase::RunningBatch) {
        st_.phase = TestPhase::RunningBatch;
        st_.batchDone = 0;
        nextStartMs_ = nowMs;
        mc_logf("[TEST] start batch target=%u", (unsigned)st_.batchTarget);
      } else {
        stopBatch_();
      }
    }
  }

  // physical buttons:
  //  A: single
  //  B: batch toggle
  if (in.btnA) {
    if (st_.phase == TestPhase::Idle) {
      st_.phase = TestPhase::RunningSingle;
      startOne_(nowMs);
    }
  }
  if (in.btnB) {
    if (st_.phase != TestPhase::RunningBatch) {
      st_.phase = TestPhase::RunningBatch;
      st_.batchDone = 0;
      nextStartMs_ = nowMs;
      mc_logf("[TEST] start batch target=%u", (unsigned)st_.batchTarget);
    } else {
      stopBatch_();
    }
  }

  // running logic
  if (waiting_) {
    // 完了判定：isBusy() が false に戻ったら “この回が終わった” とみなす
    if (!tts_->isBusy()) {
      waiting_ = false;
      onCompleted_();

      if (st_.phase == TestPhase::RunningSingle) {
        st_.phase = TestPhase::Idle;
      } else if (st_.phase == TestPhase::RunningBatch) {
        st_.batchDone++;
        nextStartMs_ = nowMs + 200; // 少し間を空ける

        if (st_.batchDone >= st_.batchTarget) {
          mc_logf("[TEST] batch done");
          st_.phase = TestPhase::Idle;
        }
      }
    }
  }

  // batch start
  if (st_.phase == TestPhase::RunningBatch && !waiting_) {
    if ((int32_t)(nextStartMs_ - nowMs) <= 0) {
      startOne_(nowMs);
    }
  }

  // refresh config snapshot
  if (tts_) {
    st_.cfg = tts_->runtimeConfig();
  }
}
