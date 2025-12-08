// src/ui_mining_core2.cpp
#include "ui_mining_core2.h"

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
  avatar_.setSpeechFont(&fonts::Font0);
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

  drawStaticFrame();
  drawTicker("booting...");
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

  // スタックチャン画面ではフルスクリーン寄りレイアウト
  avatar_.setScale(1.0f);  // 好みで後で微調整
  avatar_.setPosition(0, 0);

  // 画面切り替え直後は無言。テキストは drawStackchanScreen 側で更新
  avatar_.setSpeechText("");
  // ★ avatar_.start() は使わない（自動描画タスクは封印）
}




void UIMining::onLeaveStackchanMode() {
  in_stackchan_mode_     = false;
  stackchan_needs_clear_ = false;

  // 吹き出しを消しておく
  avatar_.setSpeechText("");

  // ダッシュボード用レイアウトに戻す（左パネル版）
  avatar_.setScale(0.45f);
  avatar_.setPosition(-12, -88);

  // ★ ここでも avatar_.stop() は呼ばない（そもそも start していない）
}





void UIMining::drawAll(const PanelData& p, const String& tickerText) {
  // 1) タッチは毎フレーム処理（間引かない）
  handlePageInput();

  // 2) ティッカーも毎フレーム回す（中の interval で速度調整）
  drawTicker(tickerText);

  // 3) 右パネルとアバターだけ間引いて描画（負荷軽減）
  static uint32_t last = 0;
  uint32_t now = millis();
  if (now - last < 80) {   // 120→80ms（ちょっとキビキビに）
    return;
  }
  last = now;

  updateLastShareClock(p);
  drawInfo(p);

#ifndef DISABLE_AVATAR
  auto& d = M5.Display;

  // ダッシュボード用の標準レイアウトを毎回セット
  avatar_.setScale(0.45f);
  avatar_.setPosition(-12, -88);

  // ダッシュボード画面では吹き出しは常に無し
  avatar_.setSpeechText("");

  d.setClipRect(0, 0, AV_W, AV_H);

  // ★ 機嫌（口・呼吸）＋ 目線／まばたきだけ反映
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

  // 最初の1フレームだけ前の画面を消す
  if (stackchan_needs_clear_) {
    d.fillScreen(BLACK);
    stackchan_needs_clear_ = false;
  }

  // スタックチャン専用レイアウト（大きめ）
  avatar_.setScale(1.0f);
  avatar_.setPosition(0, 0);

  // 機嫌（口パク）は共通ロジックを利用
  updateAvatarMood(p);

  // 目線・まばたき・呼吸を更新（ここをオリジナル準拠にする：後で書き換える）
  updateAvatarLiveliness();

  // 吹き出しテキスト更新（約4秒ごと）
  static uint32_t lastBubbleMs = 0;
  static String bubbleText;

  if (bubbleText.length() == 0 || now - lastBubbleMs > 4000) {
    lastBubbleMs = now;
    bubbleText   = buildStackchanBubble(p);     // マイニング情報から文言を生成
    avatar_.setSpeechText(bubbleText.c_str());  // 吹き出し描画はライブラリに任せる
  }

  // フルスクリーンにクリッピングして描画
  d.setClipRect(0, 0, W, H);   // W=320, H=240 を使っているはず
  avatar_.draw();
  d.clearClipRect();
}






// 吹き出しに入れるテキストをランダム生成
String UIMining::buildStackchanBubble(const PanelData& p) {
  int kind = random(0, 6);  // 0〜5

  switch (kind) {
    case 0: { // ハッシュレート
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

// ===== Static frame =====

void UIMining::drawStaticFrame() {
  auto& d = M5.Display;
  d.fillScreen(BLACK);
  d.drawFastVLine(X_INF, 0, INF_H, 0x18C3);
  d.drawFastHLine(0, Y_LOG - 1, W, 0x18C3);

  d.setFont(&fonts::Font0);
  d.setTextSize(1);
  d.setTextColor(WHITE, BLACK);
  d.setCursor(4, 4);
  d.printf("%s %s", app_name_.c_str(), app_ver_.c_str());
}

// ===== Page input =====

void UIMining::handlePageInput() {
  auto& tp = M5.Touch;
  static bool prevPressed = false;

  if (!tp.isEnabled()) {
    return;
  }

  auto det = tp.getDetail();
  bool pressed = det.isPressed();

  // デバッグ：状態変化があったときだけログ
  if (pressed != prevPressed) {
    Serial.printf("[TOUCH] pressed=%d x=%d y=%d\n",
                  static_cast<int>(pressed), det.x, det.y);
  }

  // 「押された瞬間」（立ち上がりエッジ）
  if (pressed && !prevPressed) {
    // ★タッチフィードバックのビープ
    //    どこを触っても鳴るようにしておく（ユーザー側の感覚優先）
    //    周波数 1500Hz, 長さ 50ms はお好みで調整OK
    M5.Speaker.tone(1500, 50);

    // 右パネル領域内ならページを進める
    if (det.x >= X_INF && det.x < X_INF + INF_W &&
        det.y >= 0     && det.y < INF_H) {
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



