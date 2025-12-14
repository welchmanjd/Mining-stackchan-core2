#pragma once
#include <Arduino.h>
#include "azure_tts.h"

struct TestInput {
  bool btnA = false;
  bool btnB = false;
  bool btnC = false;
  bool touchDown = false;
  int  touchX = 0;
  int  touchY = 0;
};

enum class TestPhase : uint8_t {
  Idle,
  RunningSingle,
  RunningBatch,
};

struct TestState {
  TestPhase phase = TestPhase::Idle;

  // current config snapshot (for UI)
  AzureTts::RuntimeConfig cfg;
  bool playbackEnabled = false;

  // counters
  uint32_t total = 0;
  uint32_t ok = 0;
  uint32_t fail = 0;
  uint32_t reject = 0;

  // batch
  uint16_t batchTarget = 20;
  uint16_t batchDone = 0;

  // last result (copied from AzureTts)
  AzureTts::LastResult last;

  // fetch timing stats (ok only)
  uint32_t minFetchMs = 0;
  uint32_t maxFetchMs = 0;
  uint64_t sumFetchMs = 0;   // for avg
  uint32_t okForStats = 0;

  uint32_t lastUpdateMs = 0;
};

class AppTestMode {
public:
  void begin(AzureTts* tts);

  void enter(uint32_t nowMs);
  void exit(uint32_t nowMs);

  void update(uint32_t nowMs, const TestInput& in);

  const TestState& state() const { return st_; }

private:
  void startOne_(uint32_t nowMs);
  void onCompleted_();
  void stopBatch_();

  // touch UI helpers
  bool hit_(int x, int y, int x0, int y0, int w, int h) const;

  // quick toggles
  void toggleKeepAlive_();
  void toggleChunkTimeout_();   // 15s <-> 60s
  void togglePlayback_();       // OFF <-> ON

private:
  AzureTts* tts_ = nullptr;

  TestState st_;

  // run control
  bool waiting_ = false;
  uint32_t startedAtMs_ = 0;
  uint32_t nextStartMs_ = 0;

  // for single/batch
  uint32_t activeSeq_ = 0;

  // test text
  String text_ = "Azure TTS テストです。1234。";
};
