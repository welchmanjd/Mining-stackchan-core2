#include "orchestrator.h"

void Orchestrator::init() {
  state_ = AppState::Idle;
  expectSpeakId_ = 0;
  expectRid_ = 0;
  mismatchCount_ = 0;
  nextTtsId_ = 1;
  pending_.clear();
  prevState_ = AppState::Idle;
  thinkWaitSinceMs_ = 0;
  timeoutLogged_ = false;
  LOG_EVT_INFO("EVT_ORCH_INIT", "state=%d", (int)state_);
}

Orchestrator::SpeakStartCmd Orchestrator::makeSpeakStartCmd(uint32_t rid, const String& text, OrchPrio prio, OrchKind kind) {
  SpeakStartCmd cmd;
  cmd.rid = rid;
  cmd.prio = prio;
  cmd.kind = kind;

  if (text.length() == 0) {
    cmd.valid = false;
    return cmd;
  }

  cmd.text = text;
  if (cmd.text.length() > kMaxSpeakText) {
    cmd.text = cmd.text.substring(0, kMaxSpeakText);
    LOG_EVT_INFO("EVT_ORCH_SPEAK_TRUNC", "rid=%lu len=%u->%u",
                 (unsigned long)cmd.rid,
                 (unsigned)text.length(),
                 (unsigned)cmd.text.length());
  }

  // 採番 (0 は使わない)
  uint32_t id = nextTtsId_++;
  if (nextTtsId_ == 0) nextTtsId_ = 1;
  cmd.ttsId = id;
  cmd.valid = true;

  LOG_EVT_INFO("EVT_ORCH_SPEAK_CMD", "rid=%lu tts_id=%lu prio=%d kind=%d len=%u",
               (unsigned long)cmd.rid, (unsigned long)cmd.ttsId,
               (int)cmd.prio, (int)cmd.kind, (unsigned)cmd.text.length());
  return cmd;
}

void Orchestrator::enqueueSpeakPending(const SpeakStartCmd& cmd) {
  if (!cmd.valid) {
    LOG_EVT_INFO("EVT_ORCH_DROP_INVALID", "rid=%lu", (unsigned long)cmd.rid);
    return;
  }

  if (!pending_.empty() && pending_.back().kind == cmd.kind) {
    // 同種最新置換（kind が同じときのみ）
    SpeakStartCmd replaced = pending_.back();
    pending_.back() = cmd;
    LOG_EVT_INFO("EVT_ORCH_REPLACE",
                 "old_rid=%lu old_tts_id=%lu new_rid=%lu new_tts_id=%lu kind=%d",
                 (unsigned long)replaced.rid, (unsigned long)replaced.ttsId,
                 (unsigned long)cmd.rid, (unsigned long)cmd.ttsId, (int)cmd.kind);
  } else {
    pending_.push_back(cmd);
  }

  if (pending_.size() > kMaxPending) {
    SpeakStartCmd dropped = pending_.front();
    pending_.erase(pending_.begin());
    LOG_EVT_INFO("EVT_ORCH_DROP_OLD",
                 "rid=%lu tts_id=%lu kind=%d size=%u",
                 (unsigned long)dropped.rid, (unsigned long)dropped.ttsId, (int)dropped.kind,
                 (unsigned)pending_.size());
  }
}

bool Orchestrator::hasPendingSpeak() const {
  return !pending_.empty();
}

Orchestrator::SpeakStartCmd Orchestrator::popNextPending() {
  SpeakStartCmd out;
  if (pending_.empty()) return out;
  out = pending_.front();
  pending_.erase(pending_.begin());
  LOG_EVT_INFO("EVT_ORCH_POP_PENDING",
               "rid=%lu tts_id=%lu prio=%d kind=%d len=%u size_rem=%u",
               (unsigned long)out.rid, (unsigned long)out.ttsId,
               (int)out.prio, (int)out.kind, (unsigned)out.text.length(),
               (unsigned)pending_.size());
  return out;
}

void Orchestrator::setExpectedSpeak(uint32_t speakId, uint32_t rid) {
  expectSpeakId_ = speakId;
  expectRid_ = rid;
  mismatchCount_ = 0;
  const AppState from = state_;
  state_ = AppState::ThinkWait;
  LOG_EVT_INFO("EVT_ORCH_STATE",
               "from=%d to=%d reason=expect_speak rid=%lu speak_id=%lu",
               (int)from, (int)state_,
               (unsigned long)rid, (unsigned long)speakId);
  LOG_EVT_INFO("EVT_ORCH_EXPECT_SPEAK", "expect=%lu rid=%lu",
               (unsigned long)speakId, (unsigned long)rid);
}

void Orchestrator::onAudioStart(uint32_t speakId) {
  if (expectSpeakId_ != 0 && speakId == expectSpeakId_) {
    const AppState from = state_;
    state_ = AppState::Speak;
    LOG_EVT_INFO("EVT_ORCH_STATE",
                 "from=%d to=%d reason=audio_start speak_id=%lu",
                 (int)from, (int)state_, (unsigned long)speakId);
  } else {
    LOG_EVT_INFO("EVT_ORCH_AUDIO_START_IGNORED",
                 "got=%lu expect=%lu state=%d",
                 (unsigned long)speakId,
                 (unsigned long)expectSpeakId_,
                 (int)state_);
  }
}

bool Orchestrator::onTtsDone(uint32_t gotId, bool* desyncOut) {
  if (desyncOut) *desyncOut = false;

  const uint32_t expect = expectSpeakId_;
  const bool ok = (expect != 0) && (gotId == expect);

  LOG_EVT_INFO("EVT_TTS_DONE_RX_ORCH", "got=%lu expect=%lu ok=%d",
               (unsigned long)gotId, (unsigned long)expect, ok ? 1 : 0);

  if (ok) {
    const AppState from = state_;
    state_ = AppState::Idle;
    LOG_EVT_INFO("EVT_ORCH_STATE",
                 "from=%d to=%d reason=tts_done speak_id=%lu",
                 (int)from, (int)state_, (unsigned long)gotId);
    expectSpeakId_ = 0;
    expectRid_ = 0;
    mismatchCount_ = 0;
    return true;
  }

  // mismatch
  if (expect != 0) mismatchCount_++;
  LOG_EVT_INFO("EVT_ORCH_SPEAK_MISMATCH",
               "got=%lu expect=%lu count=%u",
               (unsigned long)gotId, (unsigned long)expect, (unsigned)mismatchCount_);

  if (mismatchCount_ >= kDesyncThreshold) {
    if (desyncOut) *desyncOut = true;
  }
  return false;
}

bool Orchestrator::tick(uint32_t nowMs) {
  bool didTimeout = false;

  // 遷移検知
  if (state_ != prevState_) {
    if (state_ == AppState::ThinkWait) {
      thinkWaitSinceMs_ = nowMs;
      timeoutLogged_ = false;
    } else {
      thinkWaitSinceMs_ = 0;
      timeoutLogged_ = false;
    }
    prevState_ = state_;
  }

  // ThinkWait中のみ監視
  if (state_ == AppState::ThinkWait && !timeoutLogged_) {
    if (thinkWaitSinceMs_ != 0 && (uint32_t)(nowMs - thinkWaitSinceMs_) >= kThinkWaitTimeoutMs) {
      const size_t cleared = pending_.size();
      pending_.clear();
      expectSpeakId_ = 0;
      expectRid_ = 0;
      mismatchCount_ = 0;
      state_ = AppState::Idle;
      timeoutLogged_ = true;
      didTimeout = true;
      LOG_EVT_INFO("EVT_ORCH_TIMEOUT",
                   "state=%d elapsed_ms=%lu action=clear_pending_idle cleared=%u",
                   (int)state_, (unsigned long)(nowMs - thinkWaitSinceMs_),
                   (unsigned)cleared);
    }
  }

  return didTimeout;
}
