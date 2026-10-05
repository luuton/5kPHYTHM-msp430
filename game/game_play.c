//============================================================================//
//  game_play.c - 游戏核心实现
//
//  时间推进：曲目时间 songTime = wallNow - startWall - holdAccum。
//  暂停 → 冻结 songTime（记 pausedAt），恢复 → holdAccum += freeze lasts。
//
//  状态机：
//    PL_READY   → 3-2-1 倒计时 1200 ms，完 → PLAYING
//    PL_PLAYING → 谱面放完（songTime > length_ms + 500 ms）→ RESULT
//    PL_PAUSED  →（由 Menu 层解除）
//    PL_RESULT  →（由 Menu 层返回）
//
//  判定已全部下放 Judge_*；本模块只管理时间、MISS 扫描与边界。
//============================================================================//
#include <stdint.h>
#include "game_play.h"

#include "game_judge.h"
#include "game_chart.h"
#include "game_telem.h"

/* 由主循环维护的最近一次系统时间（供 Play_ReadyRemainMs 访问） */
uint32_t gnow_last_ms;

static uint8_t  state = PL_MENU;
static uint32_t startWallMs;
static uint32_t pausedAtMs;
static uint32_t resultAtMs;
static uint32_t songNow;
static uint16_t speedPx = 60;   /* px/s：60 px/s @60fps = 1 px/帧，从容且整像素移动 */
static uint32_t readyStartMs;

uint8_t  Play_State(void)        { return state; }
uint32_t Play_SongTimeMs(void)   { return songNow; }
uint16_t Play_SpeedPxPerS(void)  { return speedPx; }
void Play_SetSpeedPxPerS(uint16_t v) { if (v) speedPx = v; }
uint32_t Play_ResultAt(void)     { return resultAtMs; }

void Play_Start(const Chart *c)
{
    Judge_LoadChart(c);
    state = PL_READY;
    readyStartMs = 0;
    startWallMs = 0;
    songNow = 0;
}

void Play_Go(void)
{
    if (state == PL_READY) { state = PL_PLAYING; }   /* READY 到点自动进入 */
}

static void doPause(void)
{
    pausedAtMs = songNow;
    state = PL_PAUSED;
}

static void doResume(uint32_t nowMs)
{
    /* 暂停期间墙钟在走，恢复时把 startWall 往后挪，使 songNow 续上 pausedAtMs */
    startWallMs = nowMs - pausedAtMs;
    state = PL_PLAYING;
}

/* 上层（菜单）显式使用 */
void Play_ForceQuit(void)
{
    state = PL_MENU;
    songNow = 0;
}

void Play_PauseResume(uint32_t nowMs)
{
    if (state == PL_PLAYING)      doPause();
    else if (state == PL_PAUSED)  doResume(nowMs);
}

#define READY_COUNTDOWN_MS  1200u

uint32_t Play_ReadyRemainMs(void)
{
    if (state != PL_READY) return 0;
    if (readyStartMs == 0) return READY_COUNTDOWN_MS;
    if (gnow_last_ms <= readyStartMs) return READY_COUNTDOWN_MS;
    return (gnow_last_ms - readyStartMs >= READY_COUNTDOWN_MS) ? 0
           : READY_COUNTDOWN_MS - (gnow_last_ms - readyStartMs);
}

void Play_Tap(uint16_t lane, uint32_t tapTimeMs)
{
    uint32_t songT;

    if (state != PL_PLAYING) return;
    if (tapTimeMs < startWallMs) return;      /* 防御：理论上不会发生 */

    /* ⚠ 时间轴换算：主循环给的是"开机以来绝对 ms"，谱面用的是"曲目 ms"。
       必须减掉本局开始时刻，否则 dt 恒为 +几万 → 永远不命中（实测全 WRONG）。 */
    songT = tapTimeMs - startWallMs;

    {
        uint8_t res = Judge_Tap(lane, songT);
        /* 遥测：命中打印实际音符时刻与判定；空按打印 WRONG */
        Telem_Tap((uint8_t)lane, songT,
                  res ? Judge_LastHitNoteMs() : 0xFFFFFFFFu, res);
    }
}

void Play_Update(uint32_t nowMs, uint8_t edges,
                 uint8_t s1Short, uint8_t s2Short, uint8_t s1Long)
{
    (void)s1Short; (void)s2Short; (void)s1Long;

    switch (state)
    {
    case PL_READY:
        if (readyStartMs == 0) readyStartMs = nowMs;
        if (nowMs - readyStartMs >= READY_COUNTDOWN_MS)
        {
            startWallMs = nowMs;
            state = PL_PLAYING;
        }
        songNow = 0;
        break;

    case PL_PLAYING:
        songNow = nowMs - startWallMs;
        /* 输入：上升沿在 Play_Tap 里由扫描中点直接给，这里只负责 MISS */
        (void)edges;
        Judge_TimeoutMisses(songNow);
        if (Judge_NoteCount() &&
            songNow > (Judge_Chart()->length_ms + 500u))
        {
            resultAtMs = songNow;
            state = PL_RESULT;
            Telem_Result(Judge_CountOf(JR_PERFECT), Judge_CountOf(JR_GREAT),
                         Judge_CountOf(JR_GOOD),    Judge_CountOf(JR_MISS),
                         Judge_Score(), Judge_MaxCombo());
        }
        break;

    case PL_PAUSED:
        songNow = pausedAtMs;
        break;

    default:
        break;
    }
}
