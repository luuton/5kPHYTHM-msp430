//============================================================================//
//  game_judge.h - 判定：时间戳为核心，与帧率完全解耦
//============================================================================//
#ifndef GAME_JUDGE_H
#define GAME_JUDGE_H

#include <stdint.h>
#include "game_chart.h"

/* 判定窗口（ms）； HIT_MAX = 最晚可打。值可被菜单里的“判定偏移”修改 */
extern int32_t JDG_WIN_PERFECT;      /* ±30 */
extern int32_t JDG_WIN_GREAT;        /* ±60 */
extern int32_t JDG_WIN_GOOD;         /* ±100 */
extern int32_t JDG_OFFSET_MS;        /* 判定线整体偏移标定（E0.1 用） */

/* 判定得分 */
#define JDG_SCORE_PERFECT  100
#define JDG_SCORE_GREAT     70
#define JDG_SCORE_GOOD      30
#define JDG_SCORE_WRONG    -10

/* 击打处理：lane 上升沿发生时调用。
   tapTimeMs = 击打事件的时间戳（ms）；
   返回判定码：JR_PERFECT/JR_GREAT/JR_GOOD；JR_NONE(0) 表示没打到（窗口外） */
extern uint8_t Judge_Tap(uint16_t lane, uint32_t tapTimeMs);

/* 每帧调用：把已超时的音符标 MISS */
extern void Judge_TimeoutMisses(uint32_t songTimeMs);

/* 回到曲子开头重新开始：清 judged/results */
extern void Judge_Reset(void);

/* 装载谱面到工作区（拷贝 Flash → RAM 镜像，供判定/渲染同访问） */
extern void Judge_LoadChart(const Chart *c);

/* 统计：结算页用 */
extern uint16_t Judge_CountOf(uint8_t result);
extern uint32_t Judge_Score(void);
extern uint16_t Judge_Combo(void);
extern uint16_t Judge_MaxCombo(void);
extern uint8_t  Judge_FullCombo(void);

/* 当前谱面（渲染层读） */
extern const Chart *Judge_Chart(void);

/* 最近一次命中的音符时刻（遥测用；未命中为 0xFFFFFFFF） */
extern uint32_t Judge_LastHitNoteMs(void);

/* 谱面最后一个音符的时刻（ms）。用于"打完了没有"的结束判断，
   比 Chart::length_ms 更可靠（那个是手填字段，与音符列表无关）。 */
extern uint32_t Judge_LastNoteMs(void);

/* 长条尾巴：命中长条后记录结束时刻（曲目 ms），0 = 无。
   渲染层用它把尾巴继续画到时间结束；判定层不做尾判。 */
extern uint32_t Judge_HoldTailMs(uint8_t lane);

/* ---- 长条尾判 ----
   头判命中长条后该轨进入"尾判进行中"（Judge_HoldActive=1）：
     · 玩家提前松手 → Judge_HoldRelease()：按 |dt| 与尾巴时刻比较分级，
       早于 GOOD 窗口 → MISS（长条断掉）
     · 到尾时仍按着 → Judge_HoldTick()：直接 PERFECT（按到底）
   两者都会计入连击/分数，并让 Judge_LastResult 更新（屏幕上弹出判定）。 */
extern uint8_t  Judge_HoldActive(uint8_t lane);
extern uint8_t  Judge_HoldRelease(uint8_t lane, uint32_t releaseSongT);
extern uint8_t  Judge_HoldTick(uint32_t songT, uint8_t pressedMask);

/* 最近一次判定结果与发生时刻（曲目 ms），用于屏幕上实时弹出判定文字 */
extern uint8_t  Judge_LastResult(void);
extern uint32_t Judge_LastResultMs(void);

/* 最近一局 dt 均值（ms）与样本数，用于标定 JDG_OFFSET_MS */
extern int32_t  Judge_DtMeanMs(void);
extern uint16_t Judge_DtCount(void);

/* 渲染层访问 RAM 镜像（Note 含 judged/result） */
extern const Note *Judge_Notes(void);
extern uint16_t    Judge_NoteCount(void);
extern uint16_t    Judge_Head(void);

#endif /* GAME_JUDGE_H */
