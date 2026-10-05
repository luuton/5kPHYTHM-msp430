//============================================================================//
//  game_time.h - 5K 下落式节奏游戏：微秒级时基
//  Timer_A0 连续模式，SMCLK 25 MHz → 1 tick = 40 ns
//  溢出中断扩展为 32 位（约 173 s 不回绕）。本模块只提供读取。
//
//  占用资源：TIMER0_A1_VECTOR（溢出通道），全工程唯一。
//============================================================================//
#ifndef GAME_TIME_H
#define GAME_TIME_H

#include <stdint.h>

/* 初始化：TA0 = SMCLK 连续计数，允许溢出中断 */
extern void Time_Init(void);

/* 关闭 TA0（进入菜单等空闲态时调用，可选） */
extern void Time_Deinit(void);

/* 读取当前时间，单位 ns。注意：tick*40 每 4.29 s 回绕，
   只适合"短间隔求差"，不要拿绝对值做长时间累积 */
extern uint32_t Time_Now(void);

/* 读取当前时间，单位 µs（推荐用于耗时测量；回绕周期 171 s） */
extern uint32_t Time_NowUs(void);

/* 读取当前时间，单位 ms（÷25000，精度 0.04 ms） */
extern uint32_t Time_NowMs(void);

#endif /* GAME_TIME_H */
