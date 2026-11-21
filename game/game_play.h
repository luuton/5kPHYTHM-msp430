//============================================================================//
//  game_play.h - 游戏核心状态机：下落 / 判定 / 连击 / 计分
//============================================================================//
#ifndef GAME_PLAY_H
#define GAME_PLAY_H

#include <stdint.h>
#include "game_chart.h"

enum {
    PL_MENU = 0, PL_READY = 1, PL_PLAYING = 2, PL_PAUSED = 3, PL_RESULT = 4,
    PL_RESUME = 5    /* 从暂停恢复：先冻结谱面显示 5 s 倒计时，再继续下落 */
};

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
extern void Play_Tap(uint16_t lane, uint32_t tapTimeMs,
                      const uint16_t *delta);

/* 长条尾判（都带遥测）：
   · Play_HoldRelease：该轨从按下变松开的瞬间调用
   · Play_HoldTick：每次扫描调用，检查是否已到尾；按到底 → PERFECT */
extern void Play_HoldRelease(uint16_t lane, uint32_t tapTimeMs, const uint16_t *delta);
extern void Play_HoldTick(uint32_t nowMs, uint8_t pressedMask, const uint16_t *delta);

/* 暂停/继续（在 nowMs 时刻） */
extern void Play_PauseResume(uint32_t nowMs);

/* 请求恢复：不立刻继续，而是进入 PL_RESUME 倒计时（谱面冻结显示）。
   倒计时结束由 Play_Update 自动切到 PL_PLAYING。 */
extern void Play_RequestResume(uint32_t nowMs);

/* PL_RESUME 倒计时剩余 ms */
extern uint32_t Play_ResumeRemainMs(void);

/* 强制结束当前局（弃局回主菜单用） */
extern void Play_ForceQuit(void);

extern uint32_t Play_SongTimeMs(void);
extern uint16_t Play_SpeedPxPerS(void);
extern void     Play_SetSpeedPxPerS(uint16_t v);

/* READY 倒计时剩余 ms */
extern uint32_t Play_ReadyRemainMs(void);

/* 结算页进入时刻 */
extern uint32_t Play_ResultAt(void);
extern uint8_t  Play_ChartIndex(void);   /* 本局曲目下标 0..2 */

/* 最近一次帧起始系统时间（主循环维护，ReadyRemain 用） */
extern uint32_t gnow_last_ms;

#endif /* GAME_PLAY_H */
