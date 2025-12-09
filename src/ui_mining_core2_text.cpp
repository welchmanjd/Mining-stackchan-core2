// UI for Mining stackchan avatar ticker.
// Uses M5Stack-Avatar library for Stack-chan style face rendering.
// https://github.com/meganetaaan/m5stack-avatar


#include "ui_mining_core2.h"

// ===== Font helpers =====

void UIMining::prepInfoFont() {
  info_.setFont(&fonts::Font0);
  info_.setTextSize(2);
}

void UIMining::prepBodyFont() {
  info_.setFont(&fonts::Font0);
  info_.setTextSize(2);
}

void UIMining::prepHeaderFont() {
  info_.setFont(&fonts::Font0);
  info_.setTextSize(1);   // ★ヘッダだけ小さく
}

// ===== Header + dots =====

void UIMining::drawDots(const TextLayoutY& ly) {
  uint16_t active   = TFT_CYAN;
  uint16_t inactive = COL_DARK;

  const int xs[3] = { IND_X1, IND_X2, IND_X3 };
  for (int i = 0; i < 3; ++i) {
    if (i == info_page_) info_.fillCircle(xs[i], ly.ind_y, IND_R, active);
    else                 info_.drawCircle(xs[i], ly.ind_y, IND_R, inactive);
  }
}

void UIMining::drawHeader(const char* title, const TextLayoutY& ly) {
  info_.fillRect(0, ly.header, INF_W, 8, BLACK); // size1の高さ8px相当

  prepHeaderFont();
  info_.setTextColor(TFT_CYAN, BLACK);

  // ドットぶんを避けた安全幅を少し広めに確保
  int safe_w = INF_W - 30;

  String t = title;
  while (t.length() && info_.textWidth(t) > safe_w) {
    t.remove(t.length() - 1);
  }

  int tw = info_.textWidth(t);
  int x  = (safe_w - tw) / 2;
  if (x < PAD_LR) x = PAD_LR;

  info_.setCursor(x, ly.header);
  info_.print(t);

  drawDots(ly);
}

// ===== Line primitive =====

void UIMining::drawLine(int y, const char* label4, const String& value,
                        uint16_t colLabel, uint16_t colValue) {
  info_.fillRect(0, y, INF_W, CHAR_H, BLACK);

  prepBodyFont();

  info_.setTextColor(colLabel, BLACK);
  info_.setCursor(X_LABEL, y);
  String lab = String(label4);
  while (lab.length() < 4) lab += ' ';
  if (lab.length() > 4) lab = lab.substring(0, 4);
  info_.print(lab);

  info_.setTextColor(colValue, BLACK);
  info_.setCursor(X_VALUE, y);
  String v = value;
  if (v.length() > 9) v = v.substring(0, 9);
  info_.print(v);
}

// ===== Value formatters =====

String UIMining::vHash(float kh) const {
  char b[16];
  if (kh < 10.0f)       snprintf(b, sizeof(b), " %.2fkH/s", kh);
  else if (kh < 100.0f) snprintf(b, sizeof(b), " %.1fkH/s", kh);
  else                  snprintf(b, sizeof(b), " %.0fkH/s", kh);
  return String(b);
}


String UIMining::vShare(uint32_t acc, uint32_t rej, uint8_t& successOut) {
  uint32_t total   = acc + rej;
  uint8_t  success = 0;

  if (total) {
    // 四捨五入した成功率 [%]
    success = static_cast<uint8_t>((acc * 100 + total / 2) / total);
  }
  successOut = success;

  char b[16];

  // 1) まずは従来どおり "acc/rej xx%" を試す
  int n = snprintf(b, sizeof(b), "%u/%u %u%%",
                   static_cast<unsigned>(acc),
                   static_cast<unsigned>(rej),
                   static_cast<unsigned>(success));

  if (n > 9) {
    // 2) 9文字を超えるなら、% を落として "acc/rej xx" にする
    n = snprintf(b, sizeof(b), "%u/%u %u",
                 static_cast<unsigned>(acc),
                 static_cast<unsigned>(rej),
                 static_cast<unsigned>(success));
  }

  if (n > 9) {
    // 3) それでも長いケース（すごく桁が増えたとき）は
    //    "acc/rej" だけにして、成功率は色で表現
    snprintf(b, sizeof(b), "%u/%u",
             static_cast<unsigned>(acc),
             static_cast<unsigned>(rej));
  }

  return String(b);
}


String UIMining::vDiff(float diff) const {
  // simple K notation
  double v = diff;
  const char* sfx = "";
  if (v >= 1000.0) { v /= 1000.0; sfx = "K"; }
  char b[16];
  if (sfx[0]) snprintf(b, sizeof(b), " %.1f%s", v, sfx);
  else        snprintf(b, sizeof(b), " %.0f",  v);
  return String(b);
}

String UIMining::vLast(uint32_t age) const {
  uint32_t mm = age / 60;
  uint32_t ss = age % 60;
  if (mm > 99) mm = 99;
  char b[16];
  snprintf(b, sizeof(b), " %02u:%02uago",
           static_cast<unsigned>(mm),
           static_cast<unsigned>(ss));
  return String(b);
}

String UIMining::vUp(uint32_t s) const {
  uint32_t hh = s / 3600;
  s %= 3600;
  uint32_t mm = s / 60;
  uint32_t ss = s % 60;
  char b[16];
  snprintf(b, sizeof(b), " %02u:%02u:%02u",
           static_cast<unsigned>(hh),
           static_cast<unsigned>(mm),
           static_cast<unsigned>(ss));
  return String(b);
}

String UIMining::vTemp(float c) const {
  int t = static_cast<int>(roundf(c));
  char b[16];
  snprintf(b, sizeof(b), " %d C", t);
  return String(b);
}

String UIMining::vHeap() const {
  uint32_t kb = ESP.getFreeHeap() / 1024;
  char b[16];
  snprintf(b, sizeof(b), " %uKB", static_cast<unsigned>(kb));
  return String(b);
}

String UIMining::vNet(const PanelData& p) const {
  if (WiFi.status() != WL_CONNECTED) return " OFFLINE";
  if (p.poolAlive)                   return " ONLINE";
  return " CONN...";
}

String UIMining::vRssi() const {
  int rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : -100;
  char b[16];
  snprintf(b, sizeof(b), " %ddBm", rssi);
  return String(b);
}

String UIMining::vPool(const String& name) const {
  if (!name.length()) return " --";
  String s = name;
  if (s.length() > 8) s = s.substring(0, 8);
  return " " + s;
}

// ===== Color helpers =====

uint16_t UIMining::cHash(const PanelData& p) const {
  if (p.hr_kh <= 0.05f) return 0xF800;
  if (hr_ref_kh_ > 0.1f) {
    float r = p.hr_kh / hr_ref_kh_;
    if (r >= 0.90f && r <= 1.10f) return TFT_CYAN;
    if (r >= 0.70f) return WHITE;
    return 0xFD20;
  }
  return TFT_CYAN;
}

uint16_t UIMining::cShare(uint8_t s) const {
  if (s == 0)        return TFT_RED; // 完全に掘れていないときは赤

  if (s >= 95)       return 0x07E0;  // good
  if (s >= 90)       return WHITE;   // まあまあ
  return 0xFD20;                     // それ以下は黄色〜オレンジ
}


uint16_t UIMining::cSharePct(float s) const {
  if (s >= 95.0f) return 0x07E0;   // green
  if (s >= 90.0f) return WHITE;    // white
  if (s >= 70.0f) return 0xFD20;   // orange/yellow
  return 0xF800;                   // red
}


uint16_t UIMining::cLast(uint32_t age) const {
  if (age <= 30)  return 0x07E0;
  if (age <= 120) return WHITE;
  if (age <= 300) return 0xFD20;
  return 0xF800;
}

uint16_t UIMining::cTemp(float c) const {
  if (c < 55.0f) return WHITE;
  if (c < 65.0f) return 0xFD20;
  return 0xF800;
}

uint16_t UIMining::cHeap(uint32_t kb) const {
  if (kb >= 50) return WHITE;
  if (kb >= 30) return 0xFD20;
  return 0xF800;
}

uint16_t UIMining::cNet(const String& v) const {
  if (v.indexOf("ONLINE") >= 0)  return 0x07E0;
  if (v.indexOf("CONN")   >= 0)  return 0xFFE0;
  return 0xF800;
}

uint16_t UIMining::cRssi(int rssi) const {
  if (rssi >= -60) return 0x07E0;
  if (rssi >= -75) return WHITE;
  return 0xFD20;
}

uint16_t UIMining::cBatt(int pct) const {
  if (pct >= 50) return 0x07E0;
  if (pct >= 20) return 0xFFE0;
  return 0xF800;
}

// ===== Temperature / Power =====

float UIMining::readTempC() {
  float t = NAN;
  M5.Imu.getTemp(&t);
#if defined(ARDUINO_ARCH_ESP32)
  if (isnan(t) || t < -40.0f || t > 125.0f) {
    t = temperatureRead();
  }
#endif
  if (isnan(t)) t = 0.0f;
  return t;
}

// バッテリ残量 [%]（跳ね値フィルタ付き）
int UIMining::batteryPct() {
  static int last = -1;  // 初回は未設定

  int raw = static_cast<int>(M5.Power.getBatteryLevel());

  // 範囲外は前回値を使う
  if (raw < 0 || raw > 100) {
    return (last < 0) ? 0 : last;
  }

  // 初回はそのまま採用
  if (last < 0) {
    last = raw;
    return last;
  }

  // 1回で ±20%以上ドカンと変わった値はノイズ扱いで弾く
  if (abs(raw - last) > 20) {
    return last;
  }

  last = raw;
  return last;
}

// 外部給電中かどうかをざっくり判定
bool UIMining::isExternalPower() {
  // まずはライブラリの isCharging() をそのまま信じる
  if (M5.Power.isCharging()) return true;

  // Core2 だと満充電になると isCharging() が false になるので、
  // 100% 表示のときは「AC 扱い」にしてしまう（簡易ヘuristic）。
  if (batteryPct() >= 100) return true;

  return false;
}

// 表示用文字列: " 100% AC" / " 78% BAT" など
String UIMining::vBatt() {
  int  pct = batteryPct();
  bool ext = isExternalPower();
  char b[16];
  snprintf(b, sizeof(b), " %d%% %s", pct, ext ? "AC" : "BAT");
  return String(b);
}

// ===== Pages =====

void UIMining::drawPage0(const PanelData& p) {
  auto ly = computeTextLayoutY();

  drawHeader("MINING STATUS", ly);

  drawLine(ly.y1, "HASH", vHash(p.hr_kh), COL_LABEL, cHash(p));

  uint8_t success = 0;
  String shrVal = vShare(p.accepted, p.rejected, success);
  drawLine(ly.y2, "SHR ", shrVal, COL_LABEL, cShare(success));

  drawLine(ly.y3, "DIFF", vDiff(p.diff), COL_LABEL, WHITE);

  uint32_t age = lastShareAgeSec();
  drawLine(ly.y4, "LAST", vLast(age), COL_LABEL, cLast(age));
}

void UIMining::drawPage1(const PanelData& p) {
  auto ly = computeTextLayoutY();

  drawHeader("DEVICE STATUS", ly);

  drawLine(ly.y1, "UP  ", vUp(p.elapsed_s), COL_LABEL, WHITE);

  float tc = readTempC();
  drawLine(ly.y2, "TEMP", vTemp(tc), COL_LABEL, cTemp(tc));

  int pct = batteryPct();
  drawLine(ly.y3, "BATT", vBatt(), COL_LABEL, cBatt(pct));

  uint32_t free_kb = ESP.getFreeHeap() / 1024;
  drawLine(ly.y4, "HEAP", vHeap(), COL_LABEL, cHeap(free_kb));
}

void UIMining::drawPage2(const PanelData& p) {
  auto ly = computeTextLayoutY();
  drawHeader("NETWORK", ly);

  String nv = vNet(p);
  drawLine(ly.y1, "NET ", nv, COL_LABEL, cNet(nv));

  String pv;
  if (p.ping_ms < 0) {
    pv = " ---- ms";
  } else {
    char b[16];
    snprintf(b, sizeof(b), " %d ms", static_cast<int>(roundf(p.ping_ms)));
    pv = String(b);
  }
  drawLine(ly.y2, "PING", pv, COL_LABEL, WHITE);

  int rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : -100;
  drawLine(ly.y3, "WIFI", vRssi(), COL_LABEL, cRssi(rssi));

  // ★4行目はラベルのみ
  drawLine(ly.y4, "POOL", "", COL_LABEL, WHITE);

  // ★プール名は下に小さく表示
  drawPoolNameSmall(ly, p.poolName);
}

void UIMining::drawPoolNameSmall(const TextLayoutY& ly, const String& name) {
  int y = ly.y4 + CHAR_H + 6;  // 4行目の下

  // サイズ1の1行ぶんをクリア
  info_.fillRect(0, y, INF_W, 10, BLACK);

  info_.setFont(&fonts::Font0);
  info_.setTextSize(1);
  info_.setTextColor(WHITE, BLACK);

  String s = name.length() ? name : String("--");

  // 横幅に収まるまで切る（安全）
  int max_w = INF_W - PAD_LR * 2;
  while (s.length() && info_.textWidth(s) > max_w) {
    s.remove(s.length() - 1);
  }

  info_.setCursor(PAD_LR, y);
  info_.print(s);
}

// ===== Right panel draw =====

void UIMining::drawInfo(const PanelData& p) {
  // Clear only the text block area (header + 4 rows)
  info_.fillScreen(BLACK);

  switch (info_page_) {
    case 0: drawPage0(p); break;
    case 1: drawPage1(p); break;
    default: drawPage2(p); break;
  }

  info_.pushSprite(X_INF, 0);
}