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

  // ===== Avatar 初期化 =====
  // ダッシュボード用レイアウト（左 144x216 領域に収まるように）
  avatar_.setScale(0.45f);
  avatar_.setPosition(-12, -88);
  // Use a Japanese-capable font (size ~8) so bubble text renders correctly.
  avatar_.setSpeechFont(&fonts::lgfxJapanMinchoP_8);
  avatar_.setSpeechText("");   // ダッシュボードでは吹き出しは使わない


  // ===== 右パネル用スプライト =====
  info_.setColorDepth(8);
  info_.createSprite(INF_W, INF_H);  // ← enum で定義されている名前に合わせる
  info_.setTextWrap(false);

  // ===== ティッカー用スプライト =====
  tick_.setColorDepth(8);
  tick_.createSprite(W, LOG_H);      // ← ログ領域の高さは LOG_H を使う
  tick_.setTextWrap(false);


  last_page_ms_      = millis();
  last_share_ms_     = 0;
  last_total_shares_ = 0;

  ticker_offset_ = W;

  // 起動時スプラッシュを初期化
  splash_active_   = true;
  splash_start_ms_ = millis();
  splash_ready_ms_ = 0;   // 「全部OKになった時刻」をリセット

  // 最初は「WiFi Connecting」「Pool Waiting」からスタート（診断はまだ空）
  splash_wifi_text_  = "Connecting...";
  splash_pool_text_  = "Waiting";
  splash_wifi_col_   = 0xFD20;     // オレンジ
  splash_pool_col_   = COL_LABEL;  // グレー
  splash_wifi_hint_  = "";
  splash_pool_hint_  = "";

  // スプラッシュ1フレーム目を描画
  drawSplash(splash_wifi_text_,  splash_wifi_col_,
             splash_pool_text_,  splash_pool_col_,
             splash_wifi_hint_,  splash_pool_hint_);


  // ★スプラッシュ中はティッカーを消灯（黒で塗りつぶし）
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

  // 「しゃべる/黙る」状態をリセット
  stackchan_talking_        = false;
  stackchan_phase_start_ms_ = millis();
  stackchan_phase_dur_ms_   = 0;   // 次の drawStackchanScreen() で開始
  stackchan_bubble_text_    = "";

  // スタックチャン画面ではフルスクリーン寄りレイアウト
  avatar_.setScale(1.0f);
  avatar_.setPosition(0, 0);

  // 画面切り替え直後は無言。テキストは drawStackchanScreen 側で更新
  avatar_.setSpeechText("");
  // ★ avatar_.start() は使わない（自動描画タスクは封印）
}


void UIMining::onLeaveStackchanMode() {
  in_stackchan_mode_     = false;
  stackchan_needs_clear_ = false;

  // 念のため「しゃべる/黙る」状態を停止
  stackchan_talking_        = false;
  stackchan_phase_start_ms_ = 0;
  stackchan_phase_dur_ms_   = 0;
  stackchan_bubble_text_    = "";

  // 吹き出しを消しておく
  avatar_.setSpeechText("");

  // ダッシュボード用レイアウトに戻す（左パネル版）
  avatar_.setScale(0.45f);
  avatar_.setPosition(-12, -88);

  // ★ ここでも avatar_.stop() は呼ばない（そもそも start していない）
}



void UIMining::triggerAttention(uint32_t durationMs, const char* text) {
  if (durationMs == 0) {
    LOG_EVT_INFO("EVT_ATTENTION_EXIT", "attn=0");
    attention_active_   = false;
    attention_until_ms_ = 0;
    attention_text_     = attention_default_text_;   // 既存挙動維持（不要なら "" にしてもOK）

    if (in_stackchan_mode_) {
      // ★重要：avatar_.setSpeechText を直で呼ばない（defer経由に統一）
      setStackchanSpeech("");
    }
    return;
  }

  attention_active_   = true;
  attention_until_ms_ = millis() + durationMs;
  attention_text_     = (text && *text) ? String(text) : attention_default_text_;
  LOG_EVT_INFO("EVT_ATTENTION_ENTER", "attn=1 text=%s", attention_text_.c_str());

  if (in_stackchan_mode_) {
    // ★重要：avatar_.setSpeechText を直で呼ばない（defer経由に統一）
    setStackchanSpeech(attention_text_);

    // 既存仕様に合わせて保持（使ってないなら削除OK）
    stackchan_speech_text_ = attention_text_;
    stackchan_speech_seq_++;
  }
}

void UIMining::setAttentionDefaultText(const char* text) {
  attention_default_text_ = (text && *text) ? String(text) : String("WHAT?");
  if (!attention_active_) {
    attention_text_ = attention_default_text_;
  }
}



bool UIMining::isAttentionActive() const {
  if (!attention_active_) return false;
  // handle millis wrap-around safely
  return (int32_t)(attention_until_ms_ - millis()) > 0;
}



void UIMining::drawAll(const PanelData& p, const String& tickerText, bool suppressTouchBeep) {

  uint32_t now = millis();


   // ===== 起動スプラッシュの表示・遷移管理 =====
  if (splash_active_) {
    wl_status_t w = WiFi.status();
    uint32_t    dt_splash = now - splash_start_ms_;

    // ★ "Connecting", "Connecting..", ... を行ったり来たりさせる
    auto makeConnecting = [&](const char* base) -> String {
      uint32_t elapsed = now - splash_start_ms_;
      const uint32_t period = 200;  // 0.2秒ごとに変化
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

    // --- WiFi ライン ---
    String   wifiText;
    uint16_t wifiCol;
    if (w == WL_CONNECTED) {
      wifiText = "OK";
      wifiCol  = 0x07E0;    // 緑
    } else if (dt_splash < 10000) {
      wifiText = makeConnecting("Connecting");
      wifiCol  = 0xFD20;    // オレンジ
    } else if (dt_splash < 15000) {
      wifiText = makeConnecting("Retrying");
      wifiCol  = 0xFD20;    // オレンジ
    } else {
      wifiText = "NG";
      wifiCol  = 0xF800;    // 赤
    }

    // --- Pool ライン ---
    String   poolText;
    uint16_t poolCol;
    bool     wifi_ok = (w == WL_CONNECTED);

    if (!wifi_ok) {
      // WiFi がまだならプールも待機扱い
      poolText = "Waiting";
      poolCol  = COL_LABEL;           // グレー
    } else if (!p.miningEnabled) {
      // 掘らないモード（duco_user 空）
      poolText = "OFF";
      poolCol  = COL_LABEL;
    } else if (p.poolAlive) {
      // プールから仕事が来ている → マイニング可能
      poolText = "OK";
      poolCol  = 0x07E0;              // 緑
    } else if (dt_splash < 10000) {
      poolText = makeConnecting("Connecting");
      poolCol  = 0xFD20;              // オレンジ
    } else if (dt_splash < 15000) {
      poolText = makeConnecting("Retrying");
      poolCol  = 0xFD20;              // オレンジ
    } else {
      poolText = "NG";
      poolCol  = 0xF800;              // 赤
    }

    // --- 診断メッセージ（NGのときだけ出す） ---
    String wifiHint;
    if (wifiText == "NG" && p.wifiDiag.length()) {
      wifiHint = p.wifiDiag;
    } else {
      wifiHint = "";
    }

    String poolHint;
    if (poolText == "OFF") {
      poolHint = "Duco user is empty. Mining is disabled.";
    } else if ((poolText == "NG" || poolText == "Waiting") && p.poolDiag.length()) {
      poolHint = p.poolDiag;
    } else {
      poolHint = "";
    }

    // --- 内容が変わったときだけ再描画（チラつき防止） ---
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

    // スプラッシュ終了条件:
    // WiFi 接続 ＋ Pool alive ＋ 最低3秒経過 ＋
    // 「全部 OK になってから 1 秒待つ」場合だけ遷移する
    bool ok_now = (w == WL_CONNECTED) && (p.miningEnabled ? p.poolAlive : true);

    if (ok_now) {
      if (splash_ready_ms_ == 0) {
        splash_ready_ms_ = now;
      }
    } else {
      splash_ready_ms_ = 0;
    }

    bool ready =
      ok_now &&
      (now - splash_start_ms_ > 3000) &&      // スプラッシュを最低3秒は見せる
      (splash_ready_ms_ != 0) &&
      (now - splash_ready_ms_ > 1000);        // 全OKから1秒の余韻

    if (!ready) {
      // ★ OK 以外では絶対に抜けない（NG のときはこのまま）
      return;
    }

    // ここまで来たら通常画面へ
    splash_active_ = false;

    // 一度だけ通常レイアウトの枠を描いておく
    drawStaticFrame();
    // このまま下の通常描画フローに落ちる
  }


  // ===== ここから通常ダッシュボード描画 =====

  // 1) タッチは毎フレーム処理（間引かない）
  handlePageInput(suppressTouchBeep);

  // 2) ティッカーも毎フレーム回す（中の interval で速度調整）
  drawTicker(tickerText);

  // 3) 右パネルとアバターだけ間引いて描画（負荷軽減）
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
    d.fillScreen(BLACK);   // 吹き出し残像対策：必要なときだけ全面クリア
    stackchan_needs_clear_ = false;
  }

  // スタックチャン専用レイアウト（大きめ）
  avatar_.setScale(1.0f);
  // 吹き出し高さを推定して、下方向にはみ出す場合は上にオフセットする
  int bubbleLines = 1;
  for (int i = 0; i < stackchan_bubble_text_.length(); ++i) {
    if (stackchan_bubble_text_.charAt(i) == '\n') bubbleLines++;
  }
  const int bubbleHeight = 32 + bubbleLines * 16;  // 吹き出し枠 + 行高目安
  int offsetY = 0;
  const int margin = 4;          // 下端に残す余白
  const int availableH = H;      // 画面高さ
  int overflow = (bubbleHeight + margin) - availableH;
  if (overflow > 0) {
    offsetY = -overflow;
  }
  avatar_.setPosition(offsetY, 0);

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

  // ===== REPLACE START: Attention override block (disable) =====
  // NOTE: Core2 + m5stack-avatar で setSpeechText / draw 周りがハングしうるため、
  // Attention専用の上書き描画は無効化する。
  // Attention表示は setStackchanSpeech()（defer経由）に任せる。
  if (attention_active_) {
    // ここで return しない。通常描画を続行する。
  }
  // ===== REPLACE END =====


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
  // Format: trim to 20 chars and insert a manual wrap to keep the balloon narrow.
  auto formatBubble = [](const String& in) -> String {
    const size_t maxLen = 20;
    String s = in;
    if (s.length() > maxLen) {
      s = s.substring(0, maxLen);
      s += "…";  // ellipsis after trim
    }

    // Insert a newline after ~8 chars to clamp width (only if there's more than 8 chars).
    const size_t wrapPos = 8;
    if (s.length() > wrapPos) {
      String first = s.substring(0, wrapPos);
      String rest  = s.substring(wrapPos);
      s = first + "\n" + rest;
    }
    return s;
  };

  stackchan_bubble_text_ = formatBubble(text);
  stackchan_speech_desired_ = stackchan_bubble_text_;
  stackchan_speech_pending_ = true;
  // 吹き出しの描き換え時に背景をクリアするためのフラグ
  stackchan_needs_clear_ = true;
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
  int kind = random(0, 6);  // 0〜5

  switch (kind) {
    case 0: { // // ハッシュレート
      return String("HASH") + vHash(p.hr_kh);
    }
    case 1: { // 温度
      float tc = readTempC();
      return String("TEMP") + vTemp(tc);
    }
    case 2: { // バッテリー
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
        // vPool は長めなので、素の名前をそのまま出す
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
  // ヘッダ + 4行 = 5行
  const int lines = 5;

  // 行間（バランス取りの肝）
  // 8〜14くらいで好み調整できる
  const int gap = 12;

  const int block_h = lines * CHAR_H + (lines - 1) * gap;
  int top = (INF_H - block_h) / 2;

  // 端に寄りすぎ保険
  if (top < 6) top = 6;

  TextLayoutY ly;
  ly.header = top;
  ly.y1 = ly.header + CHAR_H + gap;
  ly.y2 = ly.y1 + CHAR_H + gap;
  ly.y3 = ly.y2 + CHAR_H + gap;
  ly.y4 = ly.y3 + CHAR_H + gap;

  // インジケータはヘッダ文字の縦中央に合わせる
  ly.ind_y = ly.header + (CHAR_H / 2);

  return ly;
}


void UIMining::drawSplash(const String& wifiText,  uint16_t wifiCol,
                          const String& poolText,  uint16_t poolCol,
                          const String& wifiHint,  const String& poolHint) {
  auto& d = M5.Display;

  // 画面全体はクリアしない（チラつき防止のため fillScreen は呼ばない）

  // 枠線だけ上書きしておく
  d.drawFastVLine(X_INF, 0, INF_H, 0x18C3);
  d.drawFastHLine(0, Y_LOG - 1, W, 0x18C3);

#ifndef DISABLE_AVATAR
  // 左側アバター（スプラッシュ中も軽く動かす）
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

  // 右側：タイトル + WiFi / Pool + バージョン + 診断
  info_.fillScreen(BLACK);
  info_.setFont(&fonts::Font0);

  int y = 4;

  // 大きいタイトル "Mining-Stackchan" を 2行で描画
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
  y += 6;  // タイトルとステータス群の間に隙間

  // WiFi / Pool の1グループを描く
  auto drawGroup = [&](const char* label, const String& status, uint16_t col,
                       const String& hint) {
    // ラベル行（小さめ）
    info_.setTextSize(1);
    info_.setTextColor(COL_LABEL, BLACK);
    info_.setCursor(PAD_LR, y);
    info_.print(label);
    y += 12;

    // ステータス行（大きめ・右寄せ）
    info_.setTextSize(2);
    info_.setTextColor(col, BLACK);
    int tw = info_.textWidth(status);
    int sx = INF_W - PAD_LR - tw;
    if (sx < PAD_LR) sx = PAD_LR;
    info_.setCursor(sx, y);
    info_.print(status);
    y += 22;

    // 診断メッセージ（小さめ・左寄せ／最大2行）
    if (hint.length()) {
      info_.setTextSize(1);
      info_.setTextColor(COL_LABEL, BLACK);

      int max_w = INF_W - PAD_LR * 2;

      // 単語ごとに行を詰めていく簡易ワードラップ（英語前提）
      auto fillLine = [&](String& src, String& dest) {
        dest = "";
        while (src.length()) {
          int spacePos = src.indexOf(' ');
          String word;
          if (spacePos == -1) {
            // 最後の単語
            word = src;
            src  = "";
          } else {
            // 先頭の単語 + スペースまで
            word = src.substring(0, spacePos + 1);
            src.remove(0, spacePos + 1);
          }

          String candidate = dest + word;
          if (info_.textWidth(candidate) > max_w) {
            if (dest.length() == 0) {
              // 1単語だけでオーバーする場合はそのまま切る
              dest = candidate;
            } else {
              // 入りきらなかった単語は次の行に回す
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

      // 1行目を作る
      fillLine(remaining, line1);
      // まだ文字が残っていれば2行目を作る
      if (remaining.length()) {
        fillLine(remaining, line2);
      }

      // 1行目
      if (line1.length()) {
        info_.setCursor(PAD_LR, y);
        info_.print(line1);
        y += 12;
      }

      // 2行目（あれば）
      if (line2.length()) {
        info_.setCursor(PAD_LR, y);
        info_.print(line2);
        y += 12;
      }

      y += 2;  // グループとの隙間をちょっとだけ追加
    }


    y += 4;  // グループ間の余白
  };

  drawGroup("WiFi", wifiText, wifiCol, wifiHint);
  drawGroup("Pool", poolText, poolCol, poolHint);

  // 右下にバージョン表記（例: v0.34）
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
  // 右側のパネル＆ティッカーだけメッセージに差し替える
  info_.fillScreen(BLACK);
  tick_.fillScreen(BLACK);

  int y = 70;  // だいたい中央あたりからスタート

  info_.setFont(&fonts::Font0);
  info_.setTextColor(WHITE, BLACK);

  // 1行目: "Zzz..."（ちょっと大きめ）
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

  // 2行目: メッセージ（普通サイズ）
  info_.setTextSize(1);
  drawCenter("Screen off, mining on", 14);

  // 実画面に反映
  info_.pushSprite(X_INF, 0);
  tick_.pushSprite(0, Y_LOG);
}




// ===== Static frame =====

void UIMining::drawStaticFrame() {
  auto& d = M5.Display;

  // 画面全体はクリアしない（チラつき防止のため fillScreen は呼ばない）
  // d.fillScreen(BLACK);

  // 枠線だけ上書きしておく
  d.drawFastVLine(X_INF, 0, INF_H, 0x18C3);
  d.drawFastHLine(0, Y_LOG - 1, W, 0x18C3);

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





