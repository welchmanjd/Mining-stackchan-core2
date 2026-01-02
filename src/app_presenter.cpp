// src/app_presenter.cpp

#include "app_presenter.h"

#include <WiFi.h>

#include "config.h"  // appConfig()

String buildTicker(const MiningSummary& s) {
  String t;

  // “本物の計算結果(out[20])” があるなら、それを主役にする
  if (s.workHashHex[0] != '\0') {
    // 40桁hex（SHA1の結果）
    t += s.workHashHex;

    // 何を解いてるか（prev + nonce）も本物として流す
    if (s.workSeed[0] != '\0') {
      t += "|";
      t += s.workSeed;        // prev(最大40)
      t += "|";
      t += String(s.workNonce);  // nonce（/max や diff は付けない）
    }

    // ※ここで poolName や difficulty を足さない（固定/ダッシュボードで見える値は省く）
    return t;
  }

  // フォールバック：まだスナップショットが無い時は、ログ行だけ流す（pool/diff等は入れない）
  t = s.logLine40;
  t.replace('\n', ' ');
  t.replace('\r', ' ');
  t.trim();
  return t;
}



void buildPanelData(const MiningSummary& summary, UIMining& ui, UIMining::PanelData& data) {
  const auto& cfg = appConfig();

  data.hr_kh     = summary.total_kh;
  data.accepted  = summary.accepted;
  data.rejected  = summary.rejected;

  data.rej_pct   = (summary.accepted + summary.rejected)
                     ? (100.0f * summary.rejected /
                        (float)(summary.accepted + summary.rejected))
                     : 0.0f;

  data.bestshare = -1.0f;
  data.poolAlive = summary.anyConnected;
  data.diff      = (float)summary.maxDifficulty;

  data.ping_ms   = summary.maxPingMs;
  data.miningEnabled = summary.miningEnabled;

  data.elapsed_s = ui.uptimeSeconds();
  data.sw        = cfg.app_version;
  data.fw        = ui.shortFwString();
  data.poolName  = summary.poolName;
  data.worker    = cfg.duco_rig_name;

  // WiFi 診断メッセージ
  {
    wl_status_t st = WiFi.status();
    switch (st) {
      case WL_CONNECTED:
        data.wifiDiag = "WiFi connection is OK";
        break;
      case WL_NO_SSID_AVAIL:
        data.wifiDiag = "SSID not found. Check the AP name and power.";
        break;
      case WL_CONNECT_FAILED:
        data.wifiDiag = "Check the WiFi password and encryption settings.";
        break;
      default:
        data.wifiDiag = "Check your router and signal strength.";
        break;
    }
  }

  // Pool 診断メッセージ（mining_task から）
  data.poolDiag = summary.poolDiag;
}
