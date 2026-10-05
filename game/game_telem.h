//============================================================================//
//  game_telem.h - 串口遥测（USCI_A1，P4.4/P4.5，115200-8-N-1）
//
//  重要性：判定调试窗口。回传三类数据：
//    1. 击打事件行：tap 时刻 / 音符时刻 / dt / 判定（人类可读，PC 串口助手直接看）
//    2. 帧统计：每秒一帧汇总（帧最长耗时 / CTS 全扫 min/avg/max）
//    3. 谱面结束：判定统计
//
//  发送是阻塞的（UCA1 TX FIFO 无缓冲 FIFO 只有 1 级）——一行 ~30 字节 @115200
//  ≈ 2.6 ms，只在击打瞬间（扫描窗口内）发送，帧预算安全；主循环每秒汇总行
//  4 行 ~120 B ≈ 10 ms，分摊在帧内可接受。TX 中途其余发送会被 putChar 忙等——
//  所以遥测行要短、频率要低。
//============================================================================//
#ifndef GAME_TELEM_H
#define GAME_TELEM_H

#include <stdint.h>

extern void Telem_Init(void);            /* UCA1 115200 @ SMCLK25M */

/* 击打事件入队（非阻塞，可在扫描窗口内调用）。
   noteMs = 命中的音符时刻；未命中传 0xFFFFFFFF */
extern void Telem_Tap(uint8_t lane, uint32_t tapMs, uint32_t noteMs, uint8_t result);

/* 版本横幅：开机打一行，记录本固件的全部可调参数。
   为什么需要它：多次分析串口日志时无法判断"这份 log 烧的是哪版固件"，
   导致把建模仿真建立在错误的基线上（本工程真实踩过）。
   每次改动后请把 tag 往上带一位。 */
extern void Telem_Version(const char *tag, uint16_t speedPx, int16_t alignPx,
                          int32_t winPerfect, int32_t winGreat, int32_t winGood,
                          const uint16_t *thr5);

/* 把队列里的击打行统一发出（扫描窗口外调用，会阻塞到发完） */
extern void Telem_Flush(void);

/* 帧耗时（每秒汇总一次）：最长帧 ms、CTS 扫描 min/avg/max us */
extern void Telem_FrameStats(uint16_t worstFrameMs,
                             uint16_t scanMinUs, uint16_t scanAvgUs, uint16_t scanMaxUs);

/* 结算统计 */
extern void Telem_Result(uint16_t pf, uint16_t gr, uint16_t gd, uint16_t ms,
                         uint32_t score, uint16_t maxCombo);

/* 标定行：当前 delta / 峰值 / 当前阈值（Demo_Calib 用，比读 LCD 准） */
extern void Telem_Calib(const uint16_t *delta, const uint16_t *peak, const uint16_t *thr);

/* 遥测总开关（P3 标定时可关，避免串口耗时干扰扫描） */
extern void Telem_Enable(uint8_t on);

#endif /* GAME_TELEM_H */
