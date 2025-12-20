// src/stackchan_behavior.h
#pragma once

#include <Arduino.h>

#include "ui_mining_core2.h"

// ===== Phase1 skeleton for stackchan behavior state machine =====
// State/Detect/Decide are implemented as stubs for now; Present happens in main.cpp.

enum class ReactionPriority : uint8_t {
  Low,
  Normal,
  High
};

enum class StackchanEventType : uint8_t {
  None,
  ShareAccepted,
  PoolDisconnected,
  IdleTick,
  Placeholder,
};

struct StackchanReaction {
  uint32_t            rid = 0;
  StackchanEventType  evType = StackchanEventType::None;
  ReactionPriority priority = ReactionPriority::Normal;
  m5avatar::Expression expression = m5avatar::Expression::Neutral;
  String               speechText;   // text to show/speak
  bool             speak = false;
};

class StackchanBehavior {
public:
  // Observe the latest panel snapshot and time.
  void update(const UIMining::PanelData& panel, uint32_t nowMs);

  // Notify current TTS playback state.
  void setTtsSpeaking(bool speaking);

  // Pop next reaction (Phase1: always false).
  bool popReaction(StackchanReaction* out);

  // External events (tap, button, etc.).
  void triggerEvent(StackchanEventType ev);

private:
  bool ttsSpeaking_ = false;
  uint32_t lastAccepted_ = 0;
  bool lastPoolAlive_ = false;
  bool poolInit_ = false;
  uint32_t lastEventMs_ = 0;
  bool     hasPending_ = false;
  StackchanReaction pending_;
  uint32_t nextRid_ = 1;
};
