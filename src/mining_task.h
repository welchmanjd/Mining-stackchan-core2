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
  bool     miningEnabled   = false;
};


// マイニング処理（FreeRTOS タスク群）を起動
void startMiner();

// スレッドごとの統計を集計して UI 用のサマリに詰める
void updateMiningSummary(MiningSummary& out);

// マイニングを「捨てずに」一時停止/再開する（JOB・接続は維持）
void setMiningPaused(bool paused);
bool isMiningPaused();


// ===== Mining control (for "attention mode" etc.) =====
// activeThreads:
//   0 = stop/pause (all miners idle)
//   1 = half (one miner thread)
//   2 = full (default)
// yieldProfile:
//   every N nonces -> vTaskDelay(delay_ms)
//   ※ every should be power-of-two for best speed (e.g. 1024, 256, 64)
struct MiningYieldProfile {
  uint16_t every;
  uint8_t  delay_ms;

  // NOTE: PlatformIO(ESP32/Arduino) の toolchain は gnu++11 になっていることが多い。
  // C++11 だと「メンバ初期化子付きstruct」は aggregate 扱いにならず
  // `return {1024,1};` がコンパイルできない。
  // なので constexpr ctor を用意して、どの標準でも確実に初期化できるようにする。
  constexpr MiningYieldProfile(uint16_t every_ = 1024, uint8_t delay_ms_ = 1)
    : every(every_), delay_ms(delay_ms_) {}
};

void setMiningActiveThreads(uint8_t activeThreads);
uint8_t getMiningActiveThreads();

void setMiningYieldProfile(MiningYieldProfile p);
MiningYieldProfile getMiningYieldProfile();

// Convenience presets
inline MiningYieldProfile MiningYieldNormal() { return MiningYieldProfile(1024, 1); }
inline MiningYieldProfile MiningYieldStrong() { return MiningYieldProfile(64,  3); }
