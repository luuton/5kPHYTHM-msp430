//============================================================================//
//  game_play.c - 游戏核心实现
//
//  时间推进：曲目时间 songTime = wallNow - startWall - holdAccum。
//  暂停 → 冻结 songTime（记 pausedAt），恢复 → holdAccum += freeze lasts。
//
//  状态机：
//    PL_READY   → 3-2-1 倒计时 1200 ms，完 → PLAYING
//    PL_PLAYING → 谱面放完（min(length_ms+500, 末音符+GOOD+600)，且全部判完）→ RESULT
//    PL_PAUSED  → 按 S2 请求恢复 → PL_RESUME（冻结谱面 + 5 s 倒计时）→ PL_PLAYING
//    PL_RESULT  →（由 Menu 层返回）
//
//  判定已全部下放 Judge_*；本模块只管理时间、MISS 扫描与边界。
//============================================================================//
#include <stdint.h>
#include "game_play.h"

#include "game_judge.h"
#include "game_chart.h"
#include "game_telem.h"
#include "game_save.h"

/* 由主循环维护的最近一次系统时间（供 Play_ReadyRemainMs 访问） */
uint32_t gnow_last_ms;

static uint8_t  state = PL_MENU;
static uint32_t startWallMs;
static uint32_t pausedAtMs;
static uint32_t resultAtMs;
static uint32_t songNow;
static uint16_t speedPx = 60;   /* px/s：60 px/s @60fps = 1 px/帧，从容且整像素移动 */
static uint32_t readyStartMs;
static uint32_t resumeStartMs;
static uint8_t  chartIdx = 0;      /* 当前曲目下标，结算时写最高分用 */

uint8_t  Play_State(void)        { return state; }
uint32_t Play_SongTimeMs(void)   { return songNow; }
uint16_t Play_SpeedPxPerS(void)  { return speedPx; }
void Play_SetSpeedPxPerS(uint16_t v) { if (v) speedPx = v; }
uint32_t Play_ResultAt(void)     { return resultAtMs; }
uint8_t  Play_ChartIndex(void)   { return chartIdx; }

void Play_Start(const Chart *c)
{
    uint8_t i;
    for (i = 0; i < CHART_COUNT; i++)
        if (CHART_list[i] == c) chartIdx = i;
    Save_ClearNewRecord();            /* 新的一局：清掉上一局的破纪录标志 */
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

#define RESUME_COUNTDOWN_MS  5000u

/* 从暂停恢复：先进入倒计时状态，让玩家重新看清谱面位置。
   立即恢复的问题：暂停期间玩家的手和注意力都离开了，直接开始下落
   会导致一恢复就连掉几个音符。 */
void Play_RequestResume(uint32_t nowMs)
{
    if (state == PL_PAUSED)
    {
        resumeStartMs = nowMs;
        state = PL_RESUME;
        songNow = pausedAtMs;            /* 谱面停在暂停处 */
    }
}

uint32_t Play_ResumeRemainMs(void)
{
    if (state != PL_RESUME) return 0;
    if (gnow_last_ms <= resumeStartMs) return RESUME_COUNTDOWN_MS;
    if (gnow_last_ms - resumeStartMs >= RESUME_COUNTDOWN_MS) return 0;
    return RESUME_COUNTDOWN_MS - (gnow_last_ms - resumeStartMs);
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

void Play_Tap(uint16_t lane, uint32_t tapTimeMs, const uint16_t *delta)
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
                  res ? Judge_LastHitNoteMs() : 0xFFFFFFFFu, res, delta);
    }
}

/* 长条：提前松手 → 按松手时刻与尾巴时刻之差分级（早于 GOOD 窗口算断） */
void Play_HoldRelease(uint16_t lane, uint32_t tapTimeMs, const uint16_t *delta)
{
    uint32_t songT;
    uint8_t  res;

    if (state != PL_PLAYING) return;
    if (tapTimeMs < startWallMs) return;

    songT = tapTimeMs - startWallMs;
    res = Judge_HoldRelease((uint8_t)lane, songT);
    if (res)
        Telem_Tap((uint8_t)lane, songT, Judge_HoldTailMs((uint8_t)lane), res, delta);
}

/* 长条：尾巴到点时仍按着 → PERFECT；顺手把判定发出去 */
void Play_HoldTick(uint32_t nowMs, uint8_t pressedMask, const uint16_t *delta)
{
    uint32_t songT;
    uint8_t  lane, mask;

    if (state != PL_PLAYING) return;
    if (nowMs < startWallMs) return;

    songT = nowMs - startWallMs;
    mask = Judge_HoldTick(songT, pressedMask);
    for (lane = 0; lane < 5u; lane++)
        if (mask & (1u << lane))
            /* 判定码直接由"这一刻是否还按着"决定：按着 = PERFECT，否则 MISS。
               不用 Judge_LastResult()，避免同一次调用里两条轨互相覆盖。 */
            Telem_Tap(lane, songT, Judge_HoldTailMs(lane),
                      (pressedMask & (1u << lane)) ? JR_PERFECT : JR_MISS,
                      delta);
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
        if (Judge_NoteCount())
        {
            /* 结束时刻取两者较小：
                 tLen  = length_ms + 500      —— 谱面声明长度，留 500 ms 缓冲
                 tNote = 最后音符 + GOOD + 600 —— 最后一个音符判完后再稳压 600 ms
                                                 （判定文字显示 400 ms，不能立刻切走）
               还有一个"全部判完"的兜底：若 length_ms 被填得比实际谱面短，
               光靠 tLen 会提前跳结算、尾巴上的音符永远得不到判定
               （表现为 R 行 PF+GR+GD+MISS 求和 ≠ 音符总数）。
               加上 Judge_Head() >= Judge_NoteCount() 后：
                 · length_ms 填大了 → 由 tNote 提前结束，玩家不用干等
                 · length_ms 填小了 → 兜底生效，等音符真正判完再结束 */
            uint32_t tLen  = Judge_Chart()->length_ms + 500u;
            uint32_t tNote = Judge_LastNoteMs() + (uint32_t)JDG_WIN_GOOD + 600u;
            uint32_t tEnd  = (tLen < tNote) ? tLen : tNote;

            if (songNow > tEnd && Judge_Head() >= Judge_NoteCount())
            {
                resultAtMs = songNow;
                state = PL_RESULT;
                /* 最高分：破纪录时立刻写 INFO D（约数 ms，结算页无实时要求） */
                Save_TrySetRecord(chartIdx, Judge_Score(), Judge_MaxCombo());
                Telem_Result(Judge_CountOf(JR_PERFECT), Judge_CountOf(JR_GREAT),
                             Judge_CountOf(JR_GOOD),    Judge_CountOf(JR_MISS),
                             Judge_Score(), Judge_MaxCombo());
            }
        }
        break;

    case PL_PAUSED:
        songNow = pausedAtMs;
        break;

    case PL_RESUME:
        /* 谱面冻结在暂停处，等倒计时走完再接着下 */
        songNow = pausedAtMs;
        if (nowMs - resumeStartMs >= RESUME_COUNTDOWN_MS)
        {
            doResume(nowMs);             /* startWall 搬移，songNow 从 pausedAtMs 续上 */
        }
        break;

    default:
        break;
    }
}
