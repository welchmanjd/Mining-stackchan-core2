#include "ui_mining_core2.h"
#include "logging.h"
#include "mining_task.h"

// ===== Ticker =====

void UIMining::drawTicker(const String& text) {
  // 1) まず、今回渡された文字列を整える（改行→スペース）
  String incoming = text;
  incoming.replace('\n', ' ');
  incoming.replace('\r', ' ');
  incoming.trim();  // 先頭末尾の空白を削る

  uint32_t now = millis();

  // 2) 新しい「一文」が来たら、ログバッファに継ぎ足す
  if (incoming.length() > 0 && incoming != ticker_last_) {
    ticker_last_ = incoming;

    if (ticker_log_.length() > 0) {
      ticker_log_ += "|";       // 区切り（短く統一）
    }
    ticker_log_ += incoming;

    // ログが長くなりすぎたら末尾の一部だけ残す
    const size_t MAX_LEN = 300;   // お好みで調整
    if (ticker_log_.length() > MAX_LEN) {
      ticker_log_ = ticker_log_.substring(ticker_log_.length() - MAX_LEN);
    }
  }

  // 3) 実際に流す文字列を決める
  String s = ticker_log_.length() ? ticker_log_ : incoming;
  if (s.length() == 0) {
    // 何もないときはそのまま抜ける
    tick_.fillScreen(BLACK);
    tick_.pushSprite(0, Y_LOG);
    return;
  }

  // 4) スクロール描画
  tick_.fillScreen(BLACK);
  tick_.setFont(&fonts::Font0);
  tick_.setTextSize(1);
  tick_.setTextColor(0xC618, BLACK);
  tick_.setTextWrap(false);   // 折り返し禁止

  int tw = tick_.textWidth(s);
  if (tw <= 0) {
    tick_.pushSprite(0, Y_LOG);
    return;
  }

  // 文字列ブロック1個分の幅（＋ちょっと間隔）
  const int gap  = 32;
  const int span = tw + gap;

  // ★スクロール速度
  const uint32_t interval = 10;  // msごとに
  const int      step     = 8;   // 左に動かすpx数

  if (now - last_tick_ms_ >= interval) {
    last_tick_ms_ = now;
    ticker_offset_ -= step;
    if (ticker_offset_ <= -span) {
      ticker_offset_ += span;  // 1ブロックぶん戻す（ループ）
    }
  }

  // 現在オフセット位置から画面右端までタイル状に描画
  int x = ticker_offset_;
  while (x < W) {
    tick_.setCursor(x, 8);
    tick_.print(s);
    x += span;
  }

  tick_.pushSprite(0, Y_LOG);
}

// ===== Avatar mood =====
void UIMining::updateAvatarMood(const PanelData& p) {
  uint32_t now = millis();

  // ここを変えると「どれくらいの頻度で現在moodを出すか」を調整できる
  const uint32_t kMoodPeriodicLogMs = 60 * 1000; // 60秒

  int8_t prevMood = mood_level_;

  // ===== 機嫌度の計算（-2..+2）=====
  // 更新しすぎるとチラつくので、間引いて計算（例：0.8秒ごと）
  if (now - mood_last_calc_ms_ >= 800) {
    mood_last_calc_ms_ = now;

    int8_t target = 0;

    // まずは接続状況で大きく振る
    if (WiFi.status() != WL_CONNECTED) {
      target = -2;
    } else if (!p.poolAlive) {
      target = -1;
    } else if (isMiningPaused()) {          // ★ 追加：pause中は機嫌を凍結
      target = mood_level_;                 //    （HR=0などの採掘要因で落とさない）
    } else {
      // 採掘が生きている前提で、細かく加点/減点
      int score = 0;

      // (1) 最後にシェアが動いた時間（古いほど不機嫌）
      uint32_t age = lastShareAgeSec();  // updateLastShareClock() が更新している前提
        if (age <= 120)       score += 1;   // 2分以内に動いたらご機嫌
        else if (age <= 300)  score += 0;   // 5分までは普通
        else if (age <= 900)  score -= 1;   // 15分でちょい不機嫌
        else                  score -= 2;   // 15分超えはヤバい扱い

      // (2) 成功率（Accepted / (Accepted+Rejected)）
      uint32_t total = p.accepted + p.rejected;
      if (total >= 10) {  // 少なすぎるとブレるので無視
        float success = 100.0f * (float)p.accepted / (float)total;
        if (success >= 85.0f)      score += 1;
        else if (success >= 70.0f) score += 0;
        else if (success >= 50.0f) score -= 1;
        else                       score -= 2;
      }

      // (3) ハッシュレート（止まってたら強めに不機嫌）
      if (p.hr_kh <= 0.05f) {
        score -= 2;
      } else if (hr_ref_kh_ > 0.1f) {
        float r = p.hr_kh / hr_ref_kh_;
        if (r >= 0.90f)      score += 1;
        else if (r >= 0.70f) score += 0;
        else                 score -= 1;
      }

      // score を -2..+2 に丸める
      if      (score >=  2) target =  2;
      else if (score ==  1) target =  1;
      else if (score ==  0) target =  0;
      else if (score == -1) target = -1;
      else                  target = -2;
    }

    // 急に変わると不自然なので、1段ずつ追従
    if (target > mood_level_) mood_level_++;
    else if (target < mood_level_) mood_level_--;
  }

  // ===== ログ（moodが変化したら必ず）=====
  if (mood_level_ != prevMood) {
    uint32_t age = lastShareAgeSec();
    mc_logf("[MOOD] %d -> %d (wifi=%d pool=%d age=%us A=%u R=%u HR=%.2fk ref=%.2fk)",
            (int)prevMood, (int)mood_level_,
            (int)WiFi.status(),
            p.poolAlive ? 1 : 0,
            (unsigned)age,
            (unsigned)p.accepted, (unsigned)p.rejected,
            (double)p.hr_kh, (double)hr_ref_kh_);

    // 変化ログを出した直後は、定期ログもリセット（連続出力を避ける）
    mood_last_report_ms_ = now;
  }

  // ===== ログ（定期的に現在値も出す）=====
  if (now - mood_last_report_ms_ >= kMoodPeriodicLogMs) {
    mood_last_report_ms_ = now;
    uint32_t age = lastShareAgeSec();
    mc_logf("[MOOD] current=%d (wifi=%d pool=%d age=%us A=%u R=%u HR=%.2fk ref=%.2fk)",
            (int)mood_level_,
            (int)WiFi.status(),
            p.poolAlive ? 1 : 0,
            (unsigned)age,
            (unsigned)p.accepted, (unsigned)p.rejected,
            (double)p.hr_kh, (double)hr_ref_kh_);
  }

  // ===== 口パク（喋ってる時だけ）=====
  bool talking = in_stackchan_mode_ && stackchan_talking_;
  if (talking) {
    float t = millis() * 0.02f;
    float mouth = 0.20f + 0.20f * (sinf(t) * 0.5f + 0.5f);
    avatar_.setMouthOpenRatio(mouth);
  } else {
    avatar_.setMouthOpenRatio(0.0f);
  }
}


// ===== Avatar liveliness (blink / gaze / breath) =====
//
// M5Stack-Avatar の facialLoop() をシングルスレッド用に移植したもの。
// 自然な目線移動＋まばたき＋呼吸だけをここでまとめて制御する。
//
void UIMining::updateAvatarLiveliness() {
  uint32_t now = millis();

  // bubble text showing? used to modify gaze/mouth behavior
  const bool bubble_active = in_stackchan_mode_ && (stackchan_bubble_text_.length() > 0);

  // 機嫌度で「元気さ」を決める（0.6〜1.2）
  float energy = 0.9f;      // neutral
  float eyeOpen = 1.0f;     // 開き具合（sad で少し細め）
  float gazeAmp = 1.0f;     // 目線の振れ幅
  if (mood_level_ >= 2) { energy = 1.15f; eyeOpen = 1.0f;  gazeAmp = 1.10f; }
  else if (mood_level_ == 1) { energy = 1.00f; eyeOpen = 1.0f;  gazeAmp = 1.00f; }
  else if (mood_level_ == 0) { energy = 0.90f; eyeOpen = 1.0f;  gazeAmp = 0.90f; }
  else if (mood_level_ == -1){ energy = 0.75f; eyeOpen = 0.88f; gazeAmp = 0.70f; }
  else { /* -2 */            energy = 0.60f; eyeOpen = 0.75f; gazeAmp = 0.55f; }

  // --- 共通の自然モーション状態 ---
  struct State {
    bool     initialized;

    // 目線（サッカード）
    uint32_t saccade_interval;
    uint32_t last_saccade_ms;
    float    vertical;
    float    horizontal;

    // まばたき
    uint32_t blink_interval;
    uint32_t last_blink_ms;
    bool     eye_open;

    // 呼吸（上下ゆらぎ）
    int      count;
    uint32_t last_update_ms;
  };
  static State s;

  if (!s.initialized) {
    s.initialized       = true;

    // 視線
    s.saccade_interval  = 1000;
    s.last_saccade_ms   = now;
    s.vertical          = 0.0f;
    s.horizontal        = 0.0f;

    // まばたき
    s.blink_interval    = 2500;  // とりあえず開いている時間からスタート
    s.last_blink_ms     = now;
    s.eye_open          = true;

    // 呼吸
    s.count             = 0;
    s.last_update_ms    = now;
  }

  // --- 目線のサッカード（視線ジャンプ） ---
  if (bubble_active) {
    avatar_.setGaze(0.0f, 0.0f);
  } else if (now - s.last_saccade_ms > s.saccade_interval) {
    s.vertical   = (((float)random(-1000, 1001)) / 1000.0f) * gazeAmp;
    s.horizontal = (((float)random(-1000, 1001)) / 1000.0f) * gazeAmp;

    // clamp
    if (s.vertical > 1.0f) s.vertical = 1.0f;
    if (s.vertical < -1.0f) s.vertical = -1.0f;
    if (s.horizontal > 1.0f) s.horizontal = 1.0f;
    if (s.horizontal < -1.0f) s.horizontal = -1.0f;

    avatar_.setGaze(s.vertical, s.horizontal);

    // 元気だと視線変更が少し速く、落ち込むと遅く
    if (mood_level_ >= 2)      s.saccade_interval = 350 + 80  * (uint32_t)random(0, 15);
    else if (mood_level_ == 1) s.saccade_interval = 450 + 90  * (uint32_t)random(0, 15);
    else if (mood_level_ == 0) s.saccade_interval = 500 + 100 * (uint32_t)random(0, 20);
    else                       s.saccade_interval = 900 + 150 * (uint32_t)random(0, 20);

    s.last_saccade_ms  = now;
  }


  // --- まばたき ---
  if (now - s.last_blink_ms > s.blink_interval) {
    // 目を開けている時間：2.5〜4.4秒
    // 閉じている時間：    0.3〜0.49秒
    if (s.eye_open) {
      avatar_.setEyeOpenRatio(0.0f);                       // 閉じる
      s.blink_interval = 300 + 10 * (uint32_t)random(0, 20);   // 0.3〜0.49秒
    } else {
      avatar_.setEyeOpenRatio(eyeOpen);  // 開く（機嫌で細め/ぱっちり）
      s.blink_interval = 2500 + 100 * (uint32_t)random(0, 20); // 2.5〜4.4秒
    }
    s.eye_open       = !s.eye_open;
    s.last_blink_ms  = now;
  }

  // --- 呼吸（上下ゆらぎ） ---
  uint32_t dt   = now - s.last_update_ms;
  s.last_update_ms = now;

  // だいたい 33ms ごとに1ステップ進むイメージ
  int step = dt / 33;
  if (step < 1) step = 1;
  s.count = (s.count + step) % 100;

  float breath = sinf(s.count * 2.0f * PI / 100.0f);
  avatar_.setBreath(breath * energy);

  // 口パク：吹き出し中だけ大きく開ける／それ以外は閉口
  if (bubble_active) {
    float t = millis() * 0.02f;
    float mouth = 0.35f + 0.35f * (sinf(t) * 0.5f + 0.5f);  // 0.35〜0.7 くらいで開閉
    avatar_.setMouthOpenRatio(mouth);
  } else {
    avatar_.setMouthOpenRatio(0.0f);
  }


  // === ★ 追加：顔全体の位置ゆらぎ（スタックチャン画面だけ） ===
  //
  // in_stackchan_mode_ が true のときだけ、
  // 画面内でふわふわ位置を変える。
  //
  if (in_stackchan_mode_) {
    struct BodyState {
      bool     initialized;
      float    px, py;          // 現在オフセット
      float    tx, ty;          // 目標オフセット
      uint32_t next_change_ms;  // 目標を変える時刻
    };
    static BodyState b;

    if (!b.initialized) {
      b.initialized     = true;
      b.px = b.py       = 0.0f;
      b.tx = b.ty       = 0.0f;
      b.next_change_ms  = now + 2000;
    }

    // 3〜7秒ごとに目標位置を変える
    if ((int32_t)(now - b.next_change_ms) >= 0) {
      // どのくらい動かすか（px）
      float rangeX = 20.0f * energy;  // 横 ±10px くらい default 10.0
      float rangeY = 12.0f * energy;    // 縦 ± 6px くらい default 6.0
    
      b.tx = ((float)random(-1000, 1001)) / 1000.0f * rangeX;
      b.ty = ((float)random(-1000, 1001)) / 1000.0f * rangeY;

      b.next_change_ms = now + 1000 + (uint32_t)random(0, 4000); // 3〜7秒
    }

    // 目標位置にゆっくり寄せる（なめらかなふわふわ感）
    float follow = 0.1f* energy;      // 小さいほどゆっくり 元気だと追従が少し速い default 0.04f
    b.px += (b.tx - b.px) * follow;
    b.py += (b.ty - b.py) * follow;

    // スタックチャン画面では drawStackchanScreen() でいったん (0,0) にしている想定。
    // ここでオフセットを上書きする。
    avatar_.setPosition((int)b.px, (int)b.py);
  }
}
