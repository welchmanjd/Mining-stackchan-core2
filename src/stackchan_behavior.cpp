// src/stackchan_behavior.cpp
#include "stackchan_behavior.h"
#include "logging.h"

namespace {
const char* priorityName(ReactionPriority p) {
  switch (p) {
    case ReactionPriority::Low:    return "Low";
    case ReactionPriority::Normal: return "Normal";
    case ReactionPriority::High:   return "High";
    default: return "Unknown";
  }
}

const char* eventName(StackchanEventType ev) {
  switch (ev) {
    case StackchanEventType::ShareAccepted:    return "ShareAccepted";
    case StackchanEventType::PoolDisconnected: return "PoolDisconnected";
    case StackchanEventType::IdleTick:         return "IdleTick";
    case StackchanEventType::None:             return "None";
    case StackchanEventType::Placeholder:      return "Placeholder";
    default:                                   return "Unknown";
  }
}

String shortenText(const String& s, size_t maxChars, size_t& lenOut) {
  lenOut = s.length();
  if (s.length() <= maxChars) return s;
  String out;
  for (size_t i = 0; i < maxChars && i < s.length(); ++i) {
    out += s.charAt(i);
  }
  return out;
}
}  // namespace

void StackchanBehavior::update(const UIMining::PanelData& panel, uint32_t nowMs) {
  if (!poolInit_) {
    poolInit_ = true;
    lastPoolAlive_ = panel.poolAlive;
    lastEventMs_ = nowMs;
  }

  // Detect: new accepted share
  if (panel.accepted != lastAccepted_) {
    if (panel.accepted > lastAccepted_) {
      triggerEvent(StackchanEventType::ShareAccepted);
    }
    lastAccepted_ = panel.accepted;
  }

  // Detect: pool disconnected (true -> false)
  // NOTE: "no feedback (timeout)" は「未接続」ではなく「応答が来なかった」なので、発話イベントは出さない。
  //       ダッシュボードでも喋らせない方針を維持するため、ここで PoolDisconnected 自体を抑制する。
  if (poolInit_ && lastPoolAlive_ && !panel.poolAlive) {
    const bool isTimeoutNoFeedback =
        (panel.poolDiag == "No result response from the pool.");

    if (!isTimeoutNoFeedback) {
      triggerEvent(StackchanEventType::PoolDisconnected);
    } else {
      LOG_EVT_INFO("EVT_BEH_SUPPRESS_POOL_DISCONNECT",
                  "reason=timeout_no_feedback");
    }
  }
  lastPoolAlive_ = panel.poolAlive;


  // Idle tick
  const uint32_t idleMs = 30000;
  if ((uint32_t)(nowMs - lastEventMs_) >= idleMs) {
    triggerEvent(StackchanEventType::IdleTick);
  }
}

void StackchanBehavior::setTtsSpeaking(bool speaking) {
  ttsSpeaking_ = speaking;
}

bool StackchanBehavior::popReaction(StackchanReaction* out) {
  if (!out || !hasPending_) return false;

  // If TTS is speaking, allow only >= Normal. Low is dropped with a log.
  if (ttsSpeaking_ && pending_.priority == ReactionPriority::Low) {
    LOG_EVT_INFO("EVT_BEH_DROP_LOW_WHILE_BUSY",
                 "rid=%lu type=%s prio=%s speak=%d",
                 (unsigned long)pending_.rid,
                 eventName(pending_.evType),
                 priorityName(pending_.priority),
                 pending_.speak ? 1 : 0);
    hasPending_ = false;
    return false;
  }

  *out = pending_;
  hasPending_ = false;
  return true;
}

void StackchanBehavior::triggerEvent(StackchanEventType ev) {
  // Decide -> enqueue one-slot reaction (overwrite).
  StackchanReaction r;
  bool emit = false;

  r.evType = ev;
  r.rid = nextRid_++;
  if (nextRid_ == 0) nextRid_ = 1;  // avoid 0

  switch (ev) {
    case StackchanEventType::ShareAccepted:
      r.priority   = ReactionPriority::High;
      r.expression = m5avatar::Expression::Happy;
      r.speechText = "やったね! シェア獲得だよ!";
      r.speak      = true;
      emit = true;
      break;
    case StackchanEventType::PoolDisconnected:
      r.priority   = ReactionPriority::High;
      r.expression = m5avatar::Expression::Doubt;
      r.speechText = "サーバーにつながらないみたい";
      r.speak      = true;
      emit = true;
      break;
    case StackchanEventType::IdleTick:
      r.priority   = ReactionPriority::Low;
      r.expression = m5avatar::Expression::Neutral;
      r.speechText = "......";
      r.speak      = false;
      emit = true;
      break;
    default:
      break;
  }

  if (emit) {
    size_t newLen = 0, oldLen = 0;
    const String newShort = shortenText(r.speechText, 16, newLen);

    if (!hasPending_ || r.priority >= pending_.priority) {
      if (hasPending_) {
        const String oldShort = shortenText(pending_.speechText, 16, oldLen);
        const char* reason = (r.priority > pending_.priority) ? "prio_win" : "same_prio_latest";
        LOG_EVT_INFO("EVT_BEH_REPLACE",
                     "old_rid=%lu old_type=%s old_prio=%s old_speak=%d old_len=%u old_text=%s "
                     "new_rid=%lu new_type=%s new_prio=%s new_speak=%d new_len=%u new_text=%s "
                     "reason=%s",
                     (unsigned long)pending_.rid, eventName(pending_.evType), priorityName(pending_.priority),
                     pending_.speak ? 1 : 0, (unsigned)oldLen, oldShort.c_str(),
                     (unsigned long)r.rid, eventName(r.evType), priorityName(r.priority),
                     r.speak ? 1 : 0, (unsigned)newLen, newShort.c_str(),
                     reason);
      } else {
        LOG_EVT_INFO("EVT_BEH_EMIT",
                     "rid=%lu type=%s prio=%s speak=%d len=%u text=%s",
                     (unsigned long)r.rid, eventName(r.evType), priorityName(r.priority),
                     r.speak ? 1 : 0, (unsigned)newLen, newShort.c_str());
      }
      pending_ = r;
      hasPending_ = true;
      lastEventMs_ = millis();
    } else {
      LOG_EVT_INFO("EVT_BEH_DROP",
                   "rid=%lu type=%s prio=%s speak=%d len=%u text=%s reason=prio_lower",
                   (unsigned long)r.rid, eventName(r.evType), priorityName(r.priority),
                   r.speak ? 1 : 0, (unsigned)newLen, newShort.c_str());
    }
  }
}
