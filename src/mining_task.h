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

  // ★追加: プール接続に関する診断メッセージ
  String   poolDiag;

  // ★追加: “本当に計算している” SHA1 演出用スナップショット
  // workSeed + nonce(10進) を SHA1 した結果が workHashHex（40桁hex）
  uint8_t  workThread      = 255;   // 0/1..（不明なら255）
  uint32_t workNonce       = 0;     // 現在試している nonce
  uint32_t workMaxNonce    = 0;     // difficulty*100
  uint32_t workDifficulty  = 0;     // このスナップショットの difficulty
  char     workSeed[41]    = {0};   // prev（最大40文字 + '\0'）
  char     workHashHex[41] = {0};   // out[20] を hex 化した40文字 + '\0'
};


// マイニング処理（FreeRTOS タスク群）を起動
void startMiner();

// スレッドごとの統計を集計して UI 用のサマリに詰める
void updateMiningSummary(MiningSummary& out);
