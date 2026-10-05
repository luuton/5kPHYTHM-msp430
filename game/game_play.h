//============================================================================//
//  game_play.h - 游戏核心状态机：下落 / 判定 / 连击 / 计分
//============================================================================//
#ifndef GAME_PLAY_H
#define GAME_PLAY_H

#include <stdint.h>
#include "game_chart.h"

enum { PL_MENU = 0, PL_READY = 1, PL_PLAYING = 2, PL_PAUSED = 3, PL_RESULT = 4 };

extern uint8_t Play_State(void);

/* 装载谱面并进入 READY（倒计时由 Play_Update 推进） */
extern void Play_Start(const Chart *c);

/* READY 状态里的确认（S2 短按 → COUNTDOWN → PLAYING）在 Menu 层调用 */
extern void Play_Go(void);

/* 主循环每帧调用：
   nowMs        当前系统时间（ms）
   edges        本帧触摸上升沿位图
   s1Short/s2Short/s1Long  菜单键事件（可 0；Menu 层另行处理主菜单） */
extern void Play_Update(uint32_t nowMs, uint8_t edges,
                        uint8_t s1Short, uint8_t s2Short, uint8_t s1Long);

/* 击打直接回调（输入扫描中点调用，保证时间戳精度） */
extern void Play_Tap(uint16_t lane, uint32_t tapTimeMs);

/* 暂停/继续（在 nowMs 时刻） */
extern void Play_PauseResume(uint32_t nowMs);

/* 强制结束当前局（弃局回主菜单用） */
extern void Play_ForceQuit(void);

extern uint32_t Play_SongTimeMs(void);
extern uint16_t Play_SpeedPxPerS(void);
extern void     Play_SetSpeedPxPerS(uint16_t v);

/* READY 倒计时剩余 ms */
extern uint32_t Play_ReadyRemainMs(void);

/* 结算页进入时刻 */
extern uint32_t Play_ResultAt(void);

/* 最近一次帧起始系统时间（主循环维护，ReadyRemain 用） */
extern uint32_t gnow_last_ms;

#endif /* GAME_PLAY_H */
