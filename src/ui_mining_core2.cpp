// src/ui_mining_core2.cpp
#include "ui_mining_core2.h"
#include <WiFi.h>

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

  // 起動時スプラッシュを初期化
  splash_active_   = true;
  splash_start_ms_ = millis();
  splash_ready_ms_ = 0;   // 「全部OKになった時刻」をリセット

  // 最初は「WiFi Connecting」「Pool / Core は Waiting / Checking」からスタート
  splash_wifi_text_ = "Connecting...";
  splash_pool_text_ = "Waiting";
  splash_sys_text_  = "Checking...";
  splash_wifi_col_  = 0xFD20;     // オレンジ
  splash_pool_col_  = COL_LABEL;  // グレー
  splash_sys_col_   = 0xFD20;     // オレンジ

  // スプラッシュ1フレーム目を描画
  drawSplash(splash_wifi_text_,  splash_wifi_col_,
             splash_pool_text_,  splash_pool_col_,
             splash_sys_text_,   splash_sys_col_);

  // ★スプラッシュ中はティッカーを消灯（黒で塗りつぶし）
  tick_.fillScreen(BLACK);
  tick_.pushSprite(0, Y_LOG);
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
  uint32_t now = millis();


  // ===== 起動スプラッシュの表示・遷移管理 =====
  if (splash_active_) {
    wl_status_t w = WiFi.status();

    // ★ "Connecting", "Connecting..", ... を行ったり来たりさせる
    auto makeConnecting = [&](const char* base) -> String {
      uint32_t elapsed = now - splash_start_ms_;

      // 1ステップあたりの時間（お好みで 150〜300ms くらい）
      const uint32_t period = 200;  // 0.2秒ごとに変化

      // 0,1,2,3,4,5 の6ステップで 1→2→3→4→3→2 と往復させる
      uint32_t phase = (elapsed / period) % 6;
      uint8_t dots;
      if (phase <= 3) {
        dots = 1 + phase;     // 1,2,3,4
      } else {
        dots = 6 - phase;     // 3,2
      }

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
    } else {
      uint32_t dt = now - splash_start_ms_;
      if (dt < 10000) {
        // ★ アニメーション付き "Connecting..."
        wifiText = makeConnecting("Connecting");
        wifiCol  = 0xFD20;  // オレンジ
      } else {
        wifiText = "NG";
        wifiCol  = 0xF800;  // 赤
      }
    }

    // --- Pool ライン ---
    String   poolText;
    uint16_t poolCol;
    uint32_t dt_splash = now - splash_start_ms_;

    if (w != WL_CONNECTED) {
      // WiFi がまだならプールも待機扱い
      poolText = "Waiting";
      poolCol  = COL_LABEL;           // グレー
    } else if (p.poolAlive) {
      // プールから仕事が来ている
      poolText = "OK";
      poolCol  = 0x07E0;              // 緑
    } else if (dt_splash < 15000) {
      // ★タイムアウトまではずっと「Connecting...」
      //   → NG を出したすぐ後に成功する見た目を避ける
      poolText = makeConnecting("Connecting");
      poolCol  = 0xFD20;              // オレンジ
    } else {
      // 15秒経ってもダメなら NG
      poolText = "NG";
      poolCol  = 0xF800;              // 赤
    }



    // --- Core ライン（M5Stack Core2 のざっくり自己チェック） ---
    //   ・スプラッシュ表示から 0.5秒までは "Starting"
    //   ・0.5秒を超えたら「本体とUIは起動済み」とみなして "OK"
    String   sysText;
    uint16_t sysCol;
    uint32_t dt_core = now - splash_start_ms_;
    if (dt_core < 500) {
      sysText = "Starting";
      sysCol  = 0xFD20;   // オレンジ
    } else {
      sysText = "OK";
      sysCol  = 0x07E0;   // 緑
    }


    // --- 内容が変わったときだけ再描画（チラつき防止） ---
    if (wifiText  != splash_wifi_text_  || wifiCol  != splash_wifi_col_  ||
        poolText  != splash_pool_text_  || poolCol  != splash_pool_col_  ||
        sysText   != splash_sys_text_   || sysCol   != splash_sys_col_) {

      splash_wifi_text_ = wifiText;
      splash_pool_text_ = poolText;
      splash_sys_text_  = sysText;
      splash_wifi_col_  = wifiCol;
      splash_pool_col_  = poolCol;
      splash_sys_col_   = sysCol;

      drawSplash(splash_wifi_text_, splash_wifi_col_,
                 splash_pool_text_, splash_pool_col_,
                 splash_sys_text_,  splash_sys_col_);
    }

    // スプラッシュ中もティッカーは回す（目視ログ用）
    //drawTicker(tickerText);

    // いつスプラッシュを終わらせるか：
    //  WiFi 接続 ＋ Pool alive ＋ 最低3秒経過 ＋
    //  「全部 OK になってから 1 秒待つ」か、
    //  それでもダメなら 15 秒でタイムアウト
    bool ok_now = (w == WL_CONNECTED) && p.poolAlive;

    if (ok_now) {
      if (splash_ready_ms_ == 0) {
        // 初めて全部 OK になった瞬間を記録
        splash_ready_ms_ = now;
      }
    } else {
      // どれかが NG になったらリセット（ほぼ無い想定だが念のため）
      splash_ready_ms_ = 0;
    }

    bool ready =
      ok_now &&
      (now - splash_start_ms_ > 3000) &&      // スプラッシュを最低3秒は見せる
      (splash_ready_ms_ != 0) &&
      (now - splash_ready_ms_ > 1000);        // 全OKから1秒の余韻

    bool timeout = (now - splash_start_ms_ > 15000);

    if (!ready && !timeout) {
      // まだスプラッシュ中
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
  handlePageInput();

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


void UIMining::drawSplash(const String& wifiText,  uint16_t wifiCol,
                          const String& poolText,  uint16_t poolCol,
                          const String& sysText,   uint16_t sysCol) {
  auto& d = M5.Display;

  // ★画面全体はクリアしない（チラつき防止のため fillScreen は呼ばない）

  // 枠線だけ上書きしておく（左上の文字列は表示しない）
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

  // 右側：タイトル + WiFi / Pool / Core
  info_.fillScreen(BLACK);
  info_.setFont(&fonts::Font0);

  int y = 4;

  // ★大きいタイトル "Mining-Stackchan" を 2行で描画
  info_.setTextSize(2);
  info_.setTextColor(WHITE, BLACK);

  auto drawCenter = [&](const String& s) {
    int tw = info_.textWidth(s);
    int x  = (INF_W - tw) / 2;
    if (x < PAD_LR) x = PAD_LR;
    info_.setCursor(x, y);
    info_.print(s);
    y += 18;  // 高さ + 少し行間
  };

  drawCenter("Mining-");
  drawCenter("Stackchan");
  y += 6;  // タイトルとステータス群の間にちょっと隙間

  auto drawGroup = [&](const char* label, const String& status, uint16_t col) {
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
    y += 22;  // 高さ + 行間
  };

  drawGroup("WiFi", wifiText, wifiCol);
  drawGroup("Pool", poolText, poolCol);
  drawGroup("Core", sysText,  sysCol);

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

  // 2行目: 日本語メッセージ（普通サイズ）
  info_.setTextSize(1);
  drawCenter("Screen off, mining on", 14);

  // 実画面に反映
  info_.pushSprite(X_INF, 0);
  tick_.pushSprite(0, Y_LOG);
}




// ===== Static frame =====

void UIMining::drawStaticFrame() {
  auto& d = M5.Display;

  // ★フルリフレッシュをやめる
  // d.fillScreen(BLACK);

  // 枠線だけ描画（これならチラつかない）
  d.drawFastVLine(X_INF, 0, INF_H, 0x18C3);
  d.drawFastHLine(0, Y_LOG - 1, W, 0x18C3);

  // 左上テキストも描かない
  // d.setFont(&fonts::Font0);
  // d.setTextSize(1);
  // d.setTextColor(WHITE, BLACK);
  // d.setCursor(4, 4);
  // d.printf("%s %s", app_name_.c_str(), app_ver_.c_str());
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



