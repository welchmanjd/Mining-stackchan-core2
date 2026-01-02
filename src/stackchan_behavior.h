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

  // ---- bubble-only info (no TTS) ----
  InfoPool,
  InfoPing,
  InfoHashrate,
  InfoShares,
  InfoMiningOff,

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
  void triggerEvent(StackchanEventType ev, uint32_t nowMs);

private:
  bool ttsSpeaking_ = false;

  uint32_t lastAccepted_ = 0;
  bool lastPoolAlive_ = false;
  bool poolInit_ = false;

  // IdleTick / cadence reference
  uint32_t lastEventMs_ = 0;

  // ---- periodic bubble-only info rotation (15s) ----
  uint32_t nextInfoMs_ = 0;
  uint8_t  infoIndex_  = 0;  // 0:POOL, 1:PING, 2:HR, 3:SHR

  // latest snapshot for bubble-only formatting（毎回コピーしても軽いプリミティブ中心）
  float    infoHrKh_     = 0.0f;
  float    infoPingMs_   = -1.0f;
  uint32_t infoAccepted_ = 0;
  uint32_t infoRejected_ = 0;
  String   infoPoolName_;

  // one-slot reaction queue
  bool     hasPending_ = false;
  StackchanReaction pending_;
  uint32_t nextRid_ = 1;
};
