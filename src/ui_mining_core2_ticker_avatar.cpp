#include "ui_mining_core2.h"

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
  // 口パクは「喋ってる時だけ」に寄せる
  // - ダッシュボード：常に無言（setSpeechText("")）なので口は閉じる
  // - スタックチャン画面：しゃべる/黙るフェーズに合わせて口パク
  (void)p; // 表情ロジック拡張用の引数（将来使う）

  bool talking = in_stackchan_mode_ && stackchan_talking_;
  if (talking) {
    float t = millis() * 0.02f;
    float mouth = 0.20f + 0.20f * (sinf(t) * 0.5f + 0.5f);
    avatar_.setMouthOpenRatio(mouth);
  } else {
    avatar_.setMouthOpenRatio(0.0f);
  }
  // 呼吸（ゆらぎ）は updateAvatarLiveliness() 側でまとめて行う
}


// ===== Avatar liveliness (blink / gaze / breath) =====
//
// M5Stack-Avatar の facialLoop() をシングルスレッド用に移植したもの。
// 自然な目線移動＋まばたき＋呼吸だけをここでまとめて制御する。
//
void UIMining::updateAvatarLiveliness() {
  uint32_t now = millis();

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
  if (now - s.last_saccade_ms > s.saccade_interval) {
    // [-1.0, +1.0] の範囲でランダム
    s.vertical   = ((float)random(-1000, 1001)) / 1000.0f;
    s.horizontal = ((float)random(-1000, 1001)) / 1000.0f;

    // 両目まとめて同じ方向を見る
    avatar_.setGaze(s.vertical, s.horizontal);

    // 次のサッカードまでの時間（500〜2500 ms）
    s.saccade_interval = 500 + 100 * (uint32_t)random(0, 20);
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
      avatar_.setEyeOpenRatio(1.0f);                       // 開く
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
  avatar_.setBreath(breath);

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
      float rangeX = 10.0f;   // 横 ±10px くらい
      float rangeY = 6.0f;    // 縦 ± 6px くらい

      b.tx = ((float)random(-1000, 1001)) / 1000.0f * rangeX;
      b.ty = ((float)random(-1000, 1001)) / 1000.0f * rangeY;

      b.next_change_ms = now + 3000 + (uint32_t)random(0, 4000); // 3〜7秒
    }

    // 目標位置にゆっくり寄せる（なめらかなふわふわ感）
    float follow = 0.04f;   // 小さいほどゆっくり
    b.px += (b.tx - b.px) * follow;
    b.py += (b.ty - b.py) * follow;

    // スタックチャン画面では drawStackchanScreen() でいったん (0,0) にしている想定。
    // ここでオフセットを上書きする。
    avatar_.setPosition((int)b.px, (int)b.py);
  }
}
