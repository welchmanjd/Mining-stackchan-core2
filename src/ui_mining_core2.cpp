// src/ui_mining_core2.cpp
#include "ui_mining_core2.h"
#include <WiFi.h>
#include "logging.h"

// ===== Singleton / ctor =====

UIMining& UIMining::instance() {
  static UIMining inst;
  return inst;
}

UIMining::UIMining()
  : avatar_()
  , info_(&M5.Display)
  , tick_(&M5.Display)
{
  in_stackchan_mode_    = false;
  stackchan_needs_clear_ = false;
}


// ===== Public API =====

void UIMining::begin(const char* appName, const char* appVer) {
  app_name_ = appName ? appName : "";
  app_ver_  = appVer  ? appVer  : "";

  auto& d = M5.Display;
  d.setRotation(1);
  d.setBrightness(128);

  // ===== Avatar 蛻晄悄蛹・=====
  // 繝繝・す繝･繝懊・繝臥畑繝ｬ繧､繧｢繧ｦ繝茨ｼ亥ｷｦ 144x216 鬆伜沺縺ｫ蜿弱∪繧九ｈ縺・↓・・
  avatar_.setScale(0.45f);
  avatar_.setPosition(-12, -88);
  avatar_.setSpeechFont(&fonts::Font0);
  avatar_.setSpeechText("");   // 繝繝・す繝･繝懊・繝峨〒縺ｯ蜷ｹ縺榊・縺励・菴ｿ繧上↑縺・


  // ===== 蜿ｳ繝代ロ繝ｫ逕ｨ繧ｹ繝励Λ繧､繝・=====
  info_.setColorDepth(8);
  info_.createSprite(INF_W, INF_H);  // 竊・enum 縺ｧ螳夂ｾｩ縺輔ｌ縺ｦ縺・ｋ蜷榊燕縺ｫ蜷医ｏ縺帙ｋ
  info_.setTextWrap(false);

  // ===== 繝・ぅ繝・き繝ｼ逕ｨ繧ｹ繝励Λ繧､繝・=====
  tick_.setColorDepth(8);
  tick_.createSprite(W, LOG_H);      // 竊・繝ｭ繧ｰ鬆伜沺縺ｮ鬮倥＆縺ｯ LOG_H 繧剃ｽｿ縺・
  tick_.setTextWrap(false);


  last_page_ms_      = millis();
  last_share_ms_     = 0;
  last_total_shares_ = 0;

  ticker_offset_ = W;

  // 襍ｷ蜍墓凾繧ｹ繝励Λ繝・す繝･繧貞・譛溷喧
  splash_active_   = true;
  splash_start_ms_ = millis();
  splash_ready_ms_ = 0;   // 縲悟・驛ｨOK縺ｫ縺ｪ縺｣縺滓凾蛻ｻ縲阪ｒ繝ｪ繧ｻ繝・ヨ

  // 譛蛻昴・縲係iFi Connecting縲阪訓ool Waiting縲阪°繧峨せ繧ｿ繝ｼ繝茨ｼ郁ｨｺ譁ｭ縺ｯ縺ｾ縺遨ｺ・・
  splash_wifi_text_  = "Connecting...";
  splash_pool_text_  = "Waiting";
  splash_wifi_col_   = 0xFD20;     // 繧ｪ繝ｬ繝ｳ繧ｸ
  splash_pool_col_   = COL_LABEL;  // 繧ｰ繝ｬ繝ｼ
  splash_wifi_hint_  = "";
  splash_pool_hint_  = "";

  // 繧ｹ繝励Λ繝・す繝･1繝輔Ξ繝ｼ繝逶ｮ繧呈緒逕ｻ
  drawSplash(splash_wifi_text_,  splash_wifi_col_,
             splash_pool_text_,  splash_pool_col_,
             splash_wifi_hint_,  splash_pool_hint_);


  // 笘・せ繝励Λ繝・す繝･荳ｭ縺ｯ繝・ぅ繝・き繝ｼ繧呈ｶ育・・磯ｻ偵〒蝪励ｊ縺､縺ｶ縺暦ｼ・
  tick_.fillScreen(BLACK);
  tick_.pushSprite(0, Y_LOG);
}


void UIMining::setTouchSnapshot(const TouchSnapshot& s) {
  touch_ = s;
}


String UIMining::shortFwString() const {
  return String("r25-12-06");
}

uint32_t UIMining::uptimeSeconds() const {
  return static_cast<uint32_t>(millis() / 1000);
}

void UIMining::setHashrateReference(float kh) {
  hr_ref_kh_ = kh;
}

void UIMining::setAutoPageMs(uint32_t ms) {
  auto_page_ms_ = ms;
}


void UIMining::onEnterStackchanMode() {
  in_stackchan_mode_     = true;
  stackchan_needs_clear_ = true;

  // 縲後＠繧・∋繧・鮟吶ｋ縲咲憾諷九ｒ繝ｪ繧ｻ繝・ヨ
  stackchan_talking_        = false;
  stackchan_phase_start_ms_ = millis();
  stackchan_phase_dur_ms_   = 0;   // 谺｡縺ｮ drawStackchanScreen() 縺ｧ髢句ｧ・
  stackchan_bubble_text_    = "";

  // 繧ｹ繧ｿ繝・け繝√Ε繝ｳ逕ｻ髱｢縺ｧ縺ｯ繝輔Ν繧ｹ繧ｯ繝ｪ繝ｼ繝ｳ蟇・ｊ繝ｬ繧､繧｢繧ｦ繝・
  avatar_.setScale(1.0f);
  avatar_.setPosition(0, 0);

  // 逕ｻ髱｢蛻・ｊ譖ｿ縺育峩蠕後・辟｡險縲ゅユ繧ｭ繧ｹ繝医・ drawStackchanScreen 蛛ｴ縺ｧ譖ｴ譁ｰ
  avatar_.setSpeechText("");
  // 笘・avatar_.start() 縺ｯ菴ｿ繧上↑縺・ｼ郁・蜍墓緒逕ｻ繧ｿ繧ｹ繧ｯ縺ｯ蟆∝魂・・
}


void UIMining::onLeaveStackchanMode() {
  in_stackchan_mode_     = false;
  stackchan_needs_clear_ = false;

  // 蠢ｵ縺ｮ縺溘ａ縲後＠繧・∋繧・鮟吶ｋ縲咲憾諷九ｒ蛛懈ｭ｢
  stackchan_talking_        = false;
  stackchan_phase_start_ms_ = 0;
  stackchan_phase_dur_ms_   = 0;
  stackchan_bubble_text_    = "";

  // 蜷ｹ縺榊・縺励ｒ豸医＠縺ｦ縺翫￥
  avatar_.setSpeechText("");

  // 繝繝・す繝･繝懊・繝臥畑繝ｬ繧､繧｢繧ｦ繝医↓謌ｻ縺呻ｼ亥ｷｦ繝代ロ繝ｫ迚茨ｼ・
  avatar_.setScale(0.45f);
  avatar_.setPosition(-12, -88);

  // 笘・縺薙％縺ｧ繧・avatar_.stop() 縺ｯ蜻ｼ縺ｰ縺ｪ縺・ｼ医◎繧ゅ◎繧・start 縺励※縺・↑縺・ｼ・
}

void UIMining::triggerAttention(uint32_t durationMs, const char* text) {
  if (durationMs == 0) {
    LOG_EVT_INFO("EVT_ATTENTION_EXIT", "attn=0");
    attention_active_   = false;
    attention_until_ms_ = 0;
    attention_text_     = "WHAT?";
    if (in_stackchan_mode_) {
      avatar_.setSpeechText("");
    }
    return;
  }

  attention_active_   = true;
  attention_until_ms_ = millis() + durationMs;
  attention_text_     = (text && *text) ? String(text) : String("WHAT?");
  LOG_EVT_INFO("EVT_ATTENTION_ENTER", "attn=1 text=%s", attention_text_.c_str());

  if (in_stackchan_mode_) {
    avatar_.setSpeechText(attention_text_.c_str());

    // 笘・TS逕ｨ・哂ttention縲檎匱蜍墓凾縲阪↓騾夂衍
    stackchan_speech_text_ = attention_text_;
    stackchan_speech_seq_++;
  }
}


bool UIMining::isAttentionActive() const {
  if (!attention_active_) return false;
  // handle millis wrap-around safely
  return (int32_t)(attention_until_ms_ - millis()) > 0;
}



void UIMining::drawAll(const PanelData& p, const String& tickerText, bool suppressTouchBeep) {

  uint32_t now = millis();


   // ===== 襍ｷ蜍輔せ繝励Λ繝・す繝･縺ｮ陦ｨ遉ｺ繝ｻ驕ｷ遘ｻ邂｡逅・=====
  if (splash_active_) {
    wl_status_t w = WiFi.status();
    uint32_t    dt_splash = now - splash_start_ms_;

    // 笘・"Connecting", "Connecting..", ... 繧定｡後▲縺溘ｊ譚･縺溘ｊ縺輔○繧・
    auto makeConnecting = [&](const char* base) -> String {
      uint32_t elapsed = now - splash_start_ms_;
      const uint32_t period = 200;  // 0.2遘偵＃縺ｨ縺ｫ螟牙喧
      uint32_t phase = (elapsed / period) % 6;
      uint8_t dots;
      if (phase <= 3) dots = 1 + phase;  // 1,2,3,4
      else            dots = 6 - phase;  // 3,2

      String s(base);
      for (uint8_t i = 0; i < dots; ++i) {
        s += '.';
      }
      return s;
    };

    // --- WiFi 繝ｩ繧､繝ｳ ---
    String   wifiText;
    uint16_t wifiCol;
    if (w == WL_CONNECTED) {
      wifiText = "OK";
      wifiCol  = 0x07E0;    // 邱・
    } else if (dt_splash < 10000) {
      wifiText = makeConnecting("Connecting");
      wifiCol  = 0xFD20;    // 繧ｪ繝ｬ繝ｳ繧ｸ
    } else if (dt_splash < 15000) {
      wifiText = makeConnecting("Retrying");
      wifiCol  = 0xFD20;    // 繧ｪ繝ｬ繝ｳ繧ｸ
    } else {
      wifiText = "NG";
      wifiCol  = 0xF800;    // 襍､
    }

    // --- Pool 繝ｩ繧､繝ｳ ---
    String   poolText;
    uint16_t poolCol;
    bool     wifi_ok = (w == WL_CONNECTED);

    if (!wifi_ok) {
      // WiFi 縺後∪縺縺ｪ繧峨・繝ｼ繝ｫ繧ょｾ・ｩ滓桶縺・
      poolText = "Waiting";
      poolCol  = COL_LABEL;           // 繧ｰ繝ｬ繝ｼ
    } else if (p.poolAlive) {
      // 繝励・繝ｫ縺九ｉ莉穂ｺ九′譚･縺ｦ縺・ｋ 竊・繝槭う繝九Φ繧ｰ蜿ｯ閭ｽ
      poolText = "OK";
      poolCol  = 0x07E0;              // 邱・
    } else if (dt_splash < 10000) {
      poolText = makeConnecting("Connecting");
      poolCol  = 0xFD20;              // 繧ｪ繝ｬ繝ｳ繧ｸ
    } else if (dt_splash < 15000) {
      poolText = makeConnecting("Retrying");
      poolCol  = 0xFD20;              // 繧ｪ繝ｬ繝ｳ繧ｸ
    } else {
      poolText = "NG";
      poolCol  = 0xF800;              // 襍､
    }

    // --- 險ｺ譁ｭ繝｡繝・そ繝ｼ繧ｸ・・G縺ｮ縺ｨ縺阪□縺大・縺呻ｼ・---
    String wifiHint;
    if (wifiText == "NG" && p.wifiDiag.length()) {
      wifiHint = p.wifiDiag;
    } else {
      wifiHint = "";
    }

    String poolHint;
    if ((poolText == "NG" || poolText == "Waiting") && p.poolDiag.length()) {
      poolHint = p.poolDiag;
    } else {
      poolHint = "";
    }

    // --- 蜀・ｮｹ縺悟､峨ｏ縺｣縺溘→縺阪□縺大・謠冗判・医メ繝ｩ縺､縺埼亟豁｢・・---
    if (wifiText  != splash_wifi_text_  || wifiCol  != splash_wifi_col_  ||
        poolText  != splash_pool_text_  || poolCol  != splash_pool_col_  ||
        wifiHint  != splash_wifi_hint_  || poolHint != splash_pool_hint_) {

      splash_wifi_text_  = wifiText;
      splash_pool_text_  = poolText;
      splash_wifi_col_   = wifiCol;
      splash_pool_col_   = poolCol;
      splash_wifi_hint_  = wifiHint;
      splash_pool_hint_  = poolHint;

      drawSplash(splash_wifi_text_,  splash_wifi_col_,
                 splash_pool_text_,  splash_pool_col_,
                 splash_wifi_hint_,  splash_pool_hint_);
    }

    // 繧ｹ繝励Λ繝・す繝･邨ゆｺ・擅莉ｶ:
    // WiFi 謗･邯・・・Pool alive ・・譛菴・遘堤ｵ碁℃ ・・
    // 縲悟・驛ｨ OK 縺ｫ縺ｪ縺｣縺ｦ縺九ｉ 1 遘貞ｾ・▽縲榊ｴ蜷医□縺鷹・遘ｻ縺吶ｋ
    bool ok_now = (w == WL_CONNECTED) && p.poolAlive;

    if (ok_now) {
      if (splash_ready_ms_ == 0) {
        splash_ready_ms_ = now;
      }
    } else {
      splash_ready_ms_ = 0;
    }

    bool ready =
      ok_now &&
      (now - splash_start_ms_ > 3000) &&      // 繧ｹ繝励Λ繝・す繝･繧呈怙菴・遘偵・隕九○繧・
      (splash_ready_ms_ != 0) &&
      (now - splash_ready_ms_ > 1000);        // 蜈ｨOK縺九ｉ1遘偵・菴咎渊

    if (!ready) {
      // 笘・OK 莉･螟悶〒縺ｯ邨ｶ蟇ｾ縺ｫ謚懊￠縺ｪ縺・ｼ・G 縺ｮ縺ｨ縺阪・縺薙・縺ｾ縺ｾ・・
      return;
    }

    // 縺薙％縺ｾ縺ｧ譚･縺溘ｉ騾壼ｸｸ逕ｻ髱｢縺ｸ
    splash_active_ = false;

    // 荳蠎ｦ縺縺鷹壼ｸｸ繝ｬ繧､繧｢繧ｦ繝医・譫繧呈緒縺・※縺翫￥
    drawStaticFrame();
    // 縺薙・縺ｾ縺ｾ荳九・騾壼ｸｸ謠冗判繝輔Ο繝ｼ縺ｫ關ｽ縺｡繧・
  }


  // ===== 縺薙％縺九ｉ騾壼ｸｸ繝繝・す繝･繝懊・繝画緒逕ｻ =====

  // 1) 繧ｿ繝・メ縺ｯ豈弱ヵ繝ｬ繝ｼ繝蜃ｦ逅・ｼ磯俣蠑輔°縺ｪ縺・ｼ・
  handlePageInput(suppressTouchBeep);

  // 2) 繝・ぅ繝・き繝ｼ繧よｯ弱ヵ繝ｬ繝ｼ繝蝗槭☆・井ｸｭ縺ｮ interval 縺ｧ騾溷ｺｦ隱ｿ謨ｴ・・
  drawTicker(tickerText);

  // 3) 蜿ｳ繝代ロ繝ｫ縺ｨ繧｢繝舌ち繝ｼ縺縺鷹俣蠑輔＞縺ｦ謠冗判・郁ｲ闕ｷ霆ｽ貂幢ｼ・
  static uint32_t last = 0;
  if (now - last < 80) {
    return;
  }
  last = now;

  updateLastShareClock(p);
  drawInfo(p);

#ifndef DISABLE_AVATAR
  auto& d = M5.Display;

  avatar_.setScale(0.45f);
  avatar_.setPosition(-12, -88);
  avatar_.setSpeechText("");

  d.setClipRect(0, 0, AV_W, AV_H);
  updateAvatarMood(p);
  updateAvatarLiveliness();
  avatar_.draw();
  d.clearClipRect();
#endif
}


void UIMining::drawStackchanScreen(const PanelData& p) {
  auto& d = M5.Display;
  uint32_t now = millis();

  // フレーム間引き（ダッシュボードと同じくらいの更新感）
  static uint32_t lastFrameMs = 0;
  if (now - lastFrameMs < 80) {
    return;
  }
  lastFrameMs = now;

  updateLastShareClock(p);

  // 最初の1フレームだけ前の画面を消す
  if (stackchan_needs_clear_) {
    d.fillScreen(BLACK);
    stackchan_needs_clear_ = false;
  }

  // スタックチャン専用レイアウト（大きめ）
  avatar_.setScale(1.0f);
  avatar_.setPosition(0, 0);

  // ---- UI heartbeat (log meaning: "UI draw loop alive") ----
  // Log only on attention state changes and with low-rate heartbeat.
  static uint32_t s_lastUiHbMs = 0;
  static bool s_prevAttnActive = false;
  const uint32_t UI_HEARTBEAT_MS = 5000;  // 5秒に1回だけ

  bool attnActiveNow = attention_active_ && ((int32_t)(attention_until_ms_ - now) > 0);
  bool attnChanged = (attnActiveNow != s_prevAttnActive);
  if (attnChanged || (now - s_lastUiHbMs) >= UI_HEARTBEAT_MS) {
    LOG_EVT_DEBUG("EVT_UI_HEARTBEAT", "screen=stackchan attn=%d", attnActiveNow ? 1 : 0);
    s_lastUiHbMs = now;
    s_prevAttnActive = attnActiveNow;
  }

  // ===== Attention override ("WHAT?" mode) =====
  if (attention_active_) {
    if ((int32_t)(attention_until_ms_ - now) > 0) {
      avatar_.setSpeechText(attention_text_.c_str());

      updateAvatarMood(p);
      updateAvatarLiveliness();

      d.setClipRect(0, 0, W, H);
      avatar_.draw();
      d.clearClipRect();
      return;
    }

    // timeout
    attention_active_ = false;
    avatar_.setSpeechText("");
  }

  // normal stackchan draw
  // ---- Apply deferred avatar updates (safe point) ----
  if (stackchan_expr_pending_) {
    // Avoid noisy logs: only when changed/pending.
    LOG_EVT_DEBUG("EVT_UI_AVATAR_SET_EXP", "exp=%d", (int)stackchan_expr_desired_);
    avatar_.setExpression(stackchan_expr_desired_);
    stackchan_expr_pending_ = false;
  }
  if (stackchan_speech_pending_) {
    // NOTE: This is the most suspicious freeze point; log before/after.
    LOG_EVT_INFO("EVT_UI_AVATAR_SET_SPEECH", "len=%u", (unsigned)stackchan_speech_desired_.length());
    avatar_.setSpeechText(stackchan_speech_desired_.c_str());
    LOG_EVT_INFO("EVT_UI_AVATAR_SET_SPEECH_DONE", "ok=1");
    stackchan_speech_pending_ = false;
  }

  updateAvatarMood(p);
  updateAvatarLiveliness();

  d.setClipRect(0, 0, W, H);
  avatar_.draw();
  d.clearClipRect();
}



void UIMining::setStackchanSpeech(const String& text) {
  // Defer avatar touching to drawStackchanScreen().
  // (Direct calls to avatar_.setSpeechText() here may freeze on Core2.)
  stackchan_bubble_text_ = text;
  stackchan_speech_desired_ = stackchan_bubble_text_;
  stackchan_speech_pending_ = true;
}



void UIMining::setStackchanExpression(m5avatar::Expression exp) {
  // Defer avatar touching to drawStackchanScreen().
  stackchan_expr_desired_ = exp;
  stackchan_expr_pending_ = true;
}


void UIMining::setStackchanSpeechTiming(uint32_t talkMinMs, uint32_t talkVarMs,
                                        uint32_t silentMinMs, uint32_t silentVarMs) {
  stackchan_talk_min_ms_   = talkMinMs;
  stackchan_talk_var_ms_   = talkVarMs;
  stackchan_silent_min_ms_ = silentMinMs;
  stackchan_silent_var_ms_ = silentVarMs;
}


String UIMining::buildStackchanBubble(const PanelData& p) {
  int kind = random(0, 6);  // 0縲・

  switch (kind) {
    case 0: { // 繝上ャ繧ｷ繝･繝ｬ繝ｼ繝・
      return String("HASH") + vHash(p.hr_kh);
    }
    case 1: { // 貂ｩ蠎ｦ
      float tc = readTempC();
      return String("TEMP") + vTemp(tc);
    }
    case 2: { // 繝舌ャ繝・Μ繝ｼ
      return String("BATT") + vBatt();
    }
    case 3: { // PING
      if (p.ping_ms >= 0.0f) {
        char buf[16];
        snprintf(buf, sizeof(buf), " %.0f ms", p.ping_ms);
        return String("PING") + String(buf);
      } else {
        return String("PING -- ms");
      }
    }
    case 4: { // POOL
      if (p.poolName.length()) {
        // vPool 縺ｯ髟ｷ繧√↑縺ｮ縺ｧ縲∫ｴ縺ｮ蜷榊燕繧偵◎縺ｮ縺ｾ縺ｾ蜃ｺ縺・
        return String("POOL ") + p.poolName;
      } else {
        return String("NO POOL");
      }
    }
    default: { // SHARES
      uint8_t success = 0;
      String s = vShare(p.accepted, p.rejected, success);
      return String("SHR ") + s;
    }
  }
}





// ===== Layout helper =====

UIMining::TextLayoutY UIMining::computeTextLayoutY() const {
  // 繝倥ャ繝 + 4陦・= 5陦・
  const int lines = 5;

  // 陦碁俣・医ヰ繝ｩ繝ｳ繧ｹ蜿悶ｊ縺ｮ閧晢ｼ・
  // 8縲・4縺上ｉ縺・〒螂ｽ縺ｿ隱ｿ謨ｴ縺ｧ縺阪ｋ
  const int gap = 12;

  const int block_h = lines * CHAR_H + (lines - 1) * gap;
  int top = (INF_H - block_h) / 2;

  // 遶ｯ縺ｫ蟇・ｊ縺吶℃菫晞匱
  if (top < 6) top = 6;

  TextLayoutY ly;
  ly.header = top;
  ly.y1 = ly.header + CHAR_H + gap;
  ly.y2 = ly.y1 + CHAR_H + gap;
  ly.y3 = ly.y2 + CHAR_H + gap;
  ly.y4 = ly.y3 + CHAR_H + gap;

  // 繧､繝ｳ繧ｸ繧ｱ繝ｼ繧ｿ縺ｯ繝倥ャ繝譁・ｭ励・邵ｦ荳ｭ螟ｮ縺ｫ蜷医ｏ縺帙ｋ
  ly.ind_y = ly.header + (CHAR_H / 2);

  return ly;
}


void UIMining::drawSplash(const String& wifiText,  uint16_t wifiCol,
                          const String& poolText,  uint16_t poolCol,
                          const String& wifiHint,  const String& poolHint) {
  auto& d = M5.Display;

  // 逕ｻ髱｢蜈ｨ菴薙・繧ｯ繝ｪ繧｢縺励↑縺・ｼ医メ繝ｩ縺､縺埼亟豁｢縺ｮ縺溘ａ fillScreen 縺ｯ蜻ｼ縺ｰ縺ｪ縺・ｼ・

  // 譫邱壹□縺台ｸ頑嶌縺阪＠縺ｦ縺翫￥
  d.drawFastVLine(X_INF, 0, INF_H, 0x18C3);
  d.drawFastHLine(0, Y_LOG - 1, W, 0x18C3);

#ifndef DISABLE_AVATAR
  // 蟾ｦ蛛ｴ繧｢繝舌ち繝ｼ・医せ繝励Λ繝・す繝･荳ｭ繧りｻｽ縺丞虚縺九☆・・
  PanelData p;
  avatar_.setScale(0.45f);
  avatar_.setPosition(-12, -88);
  avatar_.setSpeechText("");

  d.setClipRect(0, 0, AV_W, AV_H);
  updateAvatarMood(p);
  updateAvatarLiveliness();
  avatar_.draw();
  d.clearClipRect();
#endif

  // 蜿ｳ蛛ｴ・壹ち繧､繝医Ν + WiFi / Pool + 繝舌・繧ｸ繝ｧ繝ｳ + 險ｺ譁ｭ
  info_.fillScreen(BLACK);
  info_.setFont(&fonts::Font0);

  int y = 4;

  // 螟ｧ縺阪＞繧ｿ繧､繝医Ν "Mining-Stackchan" 繧・2陦後〒謠冗判
  info_.setTextSize(2);
  info_.setTextColor(WHITE, BLACK);

  auto drawCenter = [&](const String& s) {
    int tw = info_.textWidth(s);
    int x  = (INF_W - tw) / 2;
    if (x < PAD_LR) x = PAD_LR;
    info_.setCursor(x, y);
    info_.print(s);
    y += 18;
  };

  drawCenter("Mining-");
  drawCenter("Stackchan");
  y += 6;  // 繧ｿ繧､繝医Ν縺ｨ繧ｹ繝・・繧ｿ繧ｹ鄒､縺ｮ髢薙↓髫咎俣

  // WiFi / Pool 縺ｮ1繧ｰ繝ｫ繝ｼ繝励ｒ謠上￥
  auto drawGroup = [&](const char* label, const String& status, uint16_t col,
                       const String& hint) {
    // 繝ｩ繝吶Ν陦鯉ｼ亥ｰ上＆繧・ｼ・
    info_.setTextSize(1);
    info_.setTextColor(COL_LABEL, BLACK);
    info_.setCursor(PAD_LR, y);
    info_.print(label);
    y += 12;

    // 繧ｹ繝・・繧ｿ繧ｹ陦鯉ｼ亥､ｧ縺阪ａ繝ｻ蜿ｳ蟇・○・・
    info_.setTextSize(2);
    info_.setTextColor(col, BLACK);
    int tw = info_.textWidth(status);
    int sx = INF_W - PAD_LR - tw;
    if (sx < PAD_LR) sx = PAD_LR;
    info_.setCursor(sx, y);
    info_.print(status);
    y += 22;

    // 險ｺ譁ｭ繝｡繝・そ繝ｼ繧ｸ・亥ｰ上＆繧√・蟾ｦ蟇・○・乗怙螟ｧ2陦鯉ｼ・
    if (hint.length()) {
      info_.setTextSize(1);
      info_.setTextColor(COL_LABEL, BLACK);

      int max_w = INF_W - PAD_LR * 2;

      // 蜊倩ｪ槭＃縺ｨ縺ｫ陦後ｒ隧ｰ繧√※縺・￥邁｡譏薙Ρ繝ｼ繝峨Λ繝・・・郁恭隱槫燕謠撰ｼ・
      auto fillLine = [&](String& src, String& dest) {
        dest = "";
        while (src.length()) {
          int spacePos = src.indexOf(' ');
          String word;
          if (spacePos == -1) {
            // 譛蠕後・蜊倩ｪ・
            word = src;
            src  = "";
          } else {
            // 蜈磯ｭ縺ｮ蜊倩ｪ・+ 繧ｹ繝壹・繧ｹ縺ｾ縺ｧ
            word = src.substring(0, spacePos + 1);
            src.remove(0, spacePos + 1);
          }

          String candidate = dest + word;
          if (info_.textWidth(candidate) > max_w) {
            if (dest.length() == 0) {
              // 1蜊倩ｪ槭□縺代〒繧ｪ繝ｼ繝舌・縺吶ｋ蝣ｴ蜷医・縺昴・縺ｾ縺ｾ蛻・ｋ
              dest = candidate;
            } else {
              // 蜈･繧翫″繧峨↑縺九▲縺溷腰隱槭・谺｡縺ｮ陦後↓蝗槭☆
              src = word + src;
            }
            break;
          }
          dest = candidate;
        }
        dest.trim();
      };

      String remaining = hint;
      String line1, line2;

      // 1陦檎岼繧剃ｽ懊ｋ
      fillLine(remaining, line1);
      // 縺ｾ縺譁・ｭ励′谿九▲縺ｦ縺・ｌ縺ｰ2陦檎岼繧剃ｽ懊ｋ
      if (remaining.length()) {
        fillLine(remaining, line2);
      }

      // 1陦檎岼
      if (line1.length()) {
        info_.setCursor(PAD_LR, y);
        info_.print(line1);
        y += 12;
      }

      // 2陦檎岼・医≠繧後・・・
      if (line2.length()) {
        info_.setCursor(PAD_LR, y);
        info_.print(line2);
        y += 12;
      }

      y += 2;  // 繧ｰ繝ｫ繝ｼ繝励→縺ｮ髫咎俣繧偵■繧・▲縺ｨ縺縺題ｿｽ蜉
    }


    y += 4;  // 繧ｰ繝ｫ繝ｼ繝鈴俣縺ｮ菴咏區
  };

  drawGroup("WiFi", wifiText, wifiCol, wifiHint);
  drawGroup("Pool", poolText, poolCol, poolHint);

  // 蜿ｳ荳九↓繝舌・繧ｸ繝ｧ繝ｳ陦ｨ險假ｼ井ｾ・ v0.34・・
  info_.setTextSize(1);
  info_.setTextColor(COL_LABEL, BLACK);

  String ver = String("v") + app_ver_;
  int tw = info_.textWidth(ver);
  int vx = INF_W - PAD_LR - tw;
  int vy = INF_H - 12;
  if (vx < PAD_LR) vx = PAD_LR;

  info_.setCursor(vx, vy);
  info_.print(ver);

  info_.pushSprite(X_INF, 0);
}



void UIMining::drawSleepMessage() {
  // 蜿ｳ蛛ｴ縺ｮ繝代ロ繝ｫ・・ユ繧｣繝・き繝ｼ縺縺代Γ繝・そ繝ｼ繧ｸ縺ｫ蟾ｮ縺玲崛縺医ｋ
  info_.fillScreen(BLACK);
  tick_.fillScreen(BLACK);

  int y = 70;  // 縺縺・◆縺・ｸｭ螟ｮ縺ゅ◆繧翫°繧峨せ繧ｿ繝ｼ繝・

  info_.setFont(&fonts::Font0);
  info_.setTextColor(WHITE, BLACK);

  // 1陦檎岼: "Zzz..."・医■繧・▲縺ｨ螟ｧ縺阪ａ・・
  info_.setTextSize(2);
  auto drawCenter = [&](const String& s, int lineHeight) {
    int tw = info_.textWidth(s);
    int x  = (INF_W - tw) / 2;
    if (x < PAD_LR) x = PAD_LR;
    info_.setCursor(x, y);
    info_.print(s);
    y += lineHeight;
  };

  drawCenter("Zzz...", 18);

  // 2陦檎岼: 譌･譛ｬ隱槭Γ繝・そ繝ｼ繧ｸ・域勸騾壹し繧､繧ｺ・・
  info_.setTextSize(1);
  drawCenter("Screen off, mining on", 14);

  // 螳溽判髱｢縺ｫ蜿肴丐
  info_.pushSprite(X_INF, 0);
  tick_.pushSprite(0, Y_LOG);
}




// ===== Static frame =====

void UIMining::drawStaticFrame() {
  auto& d = M5.Display;

  // 笘・ヵ繝ｫ繝ｪ繝輔Ξ繝・す繝･繧偵ｄ繧√ｋ
  // d.fillScreen(BLACK);

  // 譫邱壹□縺第緒逕ｻ・医％繧後↑繧峨メ繝ｩ縺､縺九↑縺・ｼ・
  d.drawFastVLine(X_INF, 0, INF_H, 0x18C3);
  d.drawFastHLine(0, Y_LOG - 1, W, 0x18C3);

  // 蟾ｦ荳翫ユ繧ｭ繧ｹ繝医ｂ謠上°縺ｪ縺・
  // d.setFont(&fonts::Font0);
  // d.setTextSize(1);
  // d.setTextColor(WHITE, BLACK);
  // d.setCursor(4, 4);
  // d.printf("%s %s", app_name_.c_str(), app_ver_.c_str());
}



// ===== Page input =====

void UIMining::handlePageInput(bool suppressTouchBeep) {
  static bool prevPressed = false;

  // NOTE: Touch is read in main loop (I2C) and cached via setTouchSnapshot().
  // UI must not touch I2C to avoid rare freezes/hangs.
  if (!touch_.enabled) {
    prevPressed = false;
    return;
  }

  bool pressed = touch_.pressed;
  int x = touch_.x;
  int y = touch_.y;

  if (pressed != prevPressed) {
    Serial.printf("[TOUCH] pressed=%d x=%d y=%d\n",
                  static_cast<int>(pressed), x, y);
  }

  if (pressed && !prevPressed) {
    if (!suppressTouchBeep) {
      M5.Speaker.tone(1500, 50);
    }

    if (x >= X_INF && x < X_INF + INF_W &&
        y >= 0     && y < INF_H) {
      info_page_    = (info_page_ + 1) % 3;
      last_page_ms_ = millis();
    }
  }

  prevPressed = pressed;
}



// ===== Last share age =====

void UIMining::updateLastShareClock(const PanelData& p) {
  uint32_t total = p.accepted + p.rejected;
  uint32_t now   = millis();

  if (last_share_ms_ == 0) {
    last_share_ms_    = now;
    last_total_shares_ = total;
    return;
  }
  if (total > last_total_shares_) {
    last_total_shares_ = total;
    last_share_ms_     = now;
  }
}

uint32_t UIMining::lastShareAgeSec() const {
  if (last_share_ms_ == 0) return 99999;
  return (millis() - last_share_ms_) / 1000;
}





