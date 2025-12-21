#pragma once

#include <Arduino.h>
#include "logging.h"
#include <vector>

enum class AppState : uint8_t { Idle, React, ThinkWait, Speak, ErrorSafe };
enum class OrchPrio : uint8_t { Low = 0, Normal = 1, High = 2 };

class Orchestrator {
public:
  enum OrchKind : uint8_t {
    Unknown       = 0,
    BehaviorSpeak = 1,
  };

  struct SpeakStartCmd {
    bool valid = false;
    uint32_t ttsId = 0;
    uint32_t rid = 0;
    OrchKind kind = OrchKind::Unknown;
    String text;
    OrchPrio prio = OrchPrio::Normal;
  };

  void init();

  // Reactionを入口にまとめ、ttsId採番をここで行う
  SpeakStartCmd makeSpeakStartCmd(uint32_t rid, const String& text, OrchPrio prio, OrchKind kind = OrchKind::BehaviorSpeak);
  void enqueueSpeakPending(const SpeakStartCmd& cmd);
  bool hasPendingSpeak() const;
  SpeakStartCmd popNextPending();

  void setExpectedSpeak(uint32_t speakId, uint32_t rid);
  void onAudioStart(uint32_t speakId);
  // desyncOut: 連続ミスマッチ閾値を超えたら true
  bool onTtsDone(uint32_t gotId, bool* desyncOut = nullptr);

  AppState state() const { return state_; }
  bool tick(uint32_t nowMs);

private:
  AppState state_ = AppState::Idle;
  uint32_t expectSpeakId_ = 0;
  uint32_t expectRid_ = 0;
  uint8_t  mismatchCount_ = 0;
  static constexpr uint8_t kDesyncThreshold = 3;
  uint32_t nextTtsId_ = 1;
  static constexpr size_t kMaxSpeakText = 128;
  static constexpr uint32_t kThinkWaitTimeoutMs = 5000;

  std::vector<SpeakStartCmd> pending_;
  static constexpr size_t kMaxPending = 4;

  // ThinkWait監視用
  AppState prevState_ = AppState::Idle;
  uint32_t thinkWaitSinceMs_ = 0;
  bool timeoutLogged_ = false;
};
