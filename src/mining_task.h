// src/mining_task.h
#pragma once
#include <Arduino.h>

// マイニングスレッドから集計して UI 側に渡すための構造体
struct MiningSummary {
  // 合計ハッシュレート [kH/s]
  float    total_kh;

  // 受理・却下されたシェアの数
  uint32_t accepted;
  uint32_t rejected;

  // スレッドの中で観測された最大 ping [ms]
  float    maxPingMs = 0.0f;

  // スレッドの中で観測された最大 difficulty
  uint32_t maxDifficulty;

  // どれか1スレッドでも「接続中」なら true
  bool     anyConnected;

  // プール名（getPool API の name）
  String   poolName;

  // ログ用 40文字以内の1行メッセージ
  String   logLine40;
};

// マイニング処理（FreeRTOS タスク群）を起動
void startMiner();

// スレッドごとの統計を集計して UI 用のサマリに詰める
void updateMiningSummary(MiningSummary& out);
