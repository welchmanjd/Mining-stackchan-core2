// src/app_presenter.h
#pragma once

#include <Arduino.h>

#include "mining_task.h"     // MiningSummary
#include "ui_mining_core2.h" // UIMining / PanelData

// UI 表示用の文字列や構造体を「整形」するだけの層。
// ここは描画はしない（UIMining の draw* は main 側が呼ぶ）。

// ティッカー表示文字列を生成
String buildTicker(const MiningSummary& s);

// 右パネル/スタックチャン画面に渡す PanelData を生成
void buildPanelData(const MiningSummary& summary, UIMining& ui, UIMining::PanelData& data);
