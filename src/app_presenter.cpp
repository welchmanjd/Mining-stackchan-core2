// src/app_presenter.cpp

#include "app_presenter.h"

#include <WiFi.h>

#include "config.h"  // appConfig()

String buildTicker(const MiningSummary& s) {
  String t = s.logLine40;
  t.replace('\n', ' ');
  t.replace('\r', ' ');
  t.trim();

  // logLine40 があるなら区切りを入れる
  if (t.length() > 0) t += "|";

  t += "P:";  t += s.poolName;
  t += "|A:"; t += String(s.accepted);
  t += "|R:"; t += String(s.rejected);
  t += "|HR:"; t += String(s.total_kh, 1); t += "k";
  t += "|D:";  t += String(s.maxDifficulty);

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
