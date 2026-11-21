//============================================================================//
//  game_judge.c - 判定实现
//
//  原则（project.md §4.2、§9.2）：
//    · 每个音符只判一次
//    · 预分配 + head/lookahead 指针推进，无 O(n²)
//    · 判定用"事件时间戳"（ms），与渲染帧率解耦
//    · 击打搜索：先跳过已判和不同轨；dt = tap - note.t：
//          dt < -PERFECT → 后面的更晚 → break（注意 arrays 按 t 升序！）
//          dt >  GOOD     → 太晚 → 继续下一个（同轨早音符可能还 pending）
//          其余 → 命中，按 |dt| 分级
//
//  RAM 镜像策略：谱面在 Flash 只读，运行前拷一份到 RAM（Note 6 B × 416 = 2496 B），
//  judged/result 直接写在镜像里。
//============================================================================//
#include <stdint.h>
#include "game_judge.h"
#include "game_chart.h"

int32_t JDG_WIN_PERFECT = 30;
int32_t JDG_WIN_GREAT   = 60;
int32_t JDG_WIN_GOOD    = 100;
int32_t JDG_OFFSET_MS   = 0;            /* 判定线整体偏移：WIP 标定后设置 */

static Note      work[CH_MAX_NOTES];    /* RAM 镜像 */
static uint16_t  workCount = 0;
static const Chart *cur = 0;
static uint16_t  head = 0;              /* 最早未判定音符的索引 */

static uint16_t combo;
static uint16_t maxCombo;
static uint32_t score;
static uint16_t counts[6];              /* 按 JR_* 统计 */

/* 长条尾巴（只有头判，尾端不判定）：命中长条时记下结束时刻，
   渲染层据此继续画出还没走完的尾巴。0 = 该轨当前没有长条。 */
static uint32_t holdTailMs[5] = {0, 0, 0, 0, 0};
static uint8_t  holdActive[5] = {0, 0, 0, 0, 0};   /* 头已判、尾未判 */
static uint8_t  anyBreak;               /* 是否有过 MISS/WRONG（full combo 用） */
static uint32_t lastNoteMs = 0;         /* 谱面最后一个音符的时刻（装载时算出） */
static uint32_t lastHitNoteMs = 0xFFFFFFFFu;   /* 遥测：最近命中的音符时刻 */
static int32_t  dtSum = 0;                     /* dt 累计（含符号），用于设置界面标定 */
static uint16_t dtN   = 0;
static uint8_t  lastResult = JR_NONE;          /* 最近一次判定（供屏幕实时显示） */
static uint32_t lastResultMs = 0;              /* 该判定发生的曲目时刻 */

const Chart *Judge_Chart(void) { return cur; }

void Judge_LoadChart(const Chart *c)
{
    uint16_t i;
    cur = c;
    workCount = (c->noteCount > CH_MAX_NOTES) ? CH_MAX_NOTES : c->noteCount;
    lastNoteMs = 0;
    for (i = 0; i < workCount; i++)
    {
        work[i] = c->notes[i];          /* struct 拷贝（8 B） */
        work[i].judged = 0;
        work[i].result = JR_NONE;
        /* 取最大值而不是直接取末元素：谱面按理按时间升序，但不做假设。
           长条算到尾巴，否则结束时尾巴还没判、结算页会把它切掉。 */
        {
            uint32_t te = (uint32_t)work[i].t_ms + work[i].hold_ms;
            if (te > lastNoteMs) lastNoteMs = te;
        }
    }
    head = 0;
    for (i = 0; i < 5; i++) { holdTailMs[i] = 0; holdActive[i] = 0; }
    combo = maxCombo = 0;
    score = 0;
    anyBreak = 0;
    for (i = 0; i < 6; i++) counts[i] = 0;
}

void Judge_Reset(void)
{
    uint16_t i;
    for (i = 0; i < workCount; i++)
    {
        work[i].judged = 0;
        work[i].result = JR_NONE;
    }
    head = 0;
    for (i = 0; i < 5; i++) { holdTailMs[i] = 0; holdActive[i] = 0; }
    combo = maxCombo = 0;
    score = 0;
    anyBreak = 0;
    lastHitNoteMs = 0xFFFFFFFFu;
    dtSum = 0; dtN = 0;
    lastResult = JR_NONE; lastResultMs = 0;
    for (i = 0; i < 6; i++) counts[i] = 0;
}

static uint8_t grade_of(int32_t adt)   /* adt = |dt| */
{
    if (adt <= JDG_WIN_PERFECT) return JR_PERFECT;
    if (adt <= JDG_WIN_GREAT)   return JR_GREAT;
    return JR_GOOD;
}

static void account(uint8_t result, uint8_t lane)
{
    int16_t pts = 0;
    counts[result]++;
    switch (result)
    {
    case JR_PERFECT: pts = JDG_SCORE_PERFECT;  combo++;        break;
    case JR_GREAT:   pts = JDG_SCORE_GREAT;    combo++;        break;
    case JR_GOOD:    pts = JDG_SCORE_GOOD;     combo++;        break;
    /* maxCombo 修复：每次 combo 变化后立即记录（原实现从未更新 → 恒 0） */

    case JR_MISS:    pts = 0;                  combo = 0; anyBreak = 1; break;
    case JR_WRONG:   pts = JDG_SCORE_WRONG;    combo = 0; anyBreak = 1; break;
    default:         pts = 0;                  break;
    }
    if (combo > maxCombo) maxCombo = combo;
    if (pts > 0)
    {
        /* combo 加成：1 + min(combo,100)/100（combo 结算前的值） */
        score += (uint32_t)pts * (100u + (combo < 100u ? combo : 100u)) / 100u;
    }
    (void)lane;
}

/* 单次击打搜索 */
uint8_t Judge_Tap(uint16_t lane, uint32_t tapTimeMs)
{
    int32_t tapT = (int32_t)tapTimeMs + JDG_OFFSET_MS;
    uint16_t n = head;
    uint16_t ahead = 0;
    uint8_t hit = 0;

    while (n < workCount && ahead < 24)      /* LOOKAHEAD = 24 */
    {
        if (work[n].judged || work[n].lane != lane)
        {
            n++; ahead++;
            continue;
        }
        {
            int32_t dt = tapT - (int32_t)work[n].t_ms;
            if (dt < -JDG_WIN_GOOD) break;   /* 音符还远没到，后面的更晚 → 结束 */
            if (dt > JDG_WIN_GOOD)           /* 已太晚且后面的更晚 → 结束（等 TimeoutMisses） */
            {
                break;
            }
            work[n].judged = 1;
            work[n].result = grade_of(dt < 0 ? -dt : dt);
            if (work[n].hold_ms)
            {
                holdTailMs[lane]  = (uint32_t)work[n].t_ms + work[n].hold_ms;
                holdActive[lane]  = 1;          /* 开始尾判 */
            }
            lastHitNoteMs = work[n].t_ms;
            dtSum += dt;                       /* 记录带符号的 dt，供均值标定 */
            dtN++;
            lastResult   = work[n].result;
            lastResultMs = tapTimeMs;          /* 用原始曲目时刻，便于渲染层比较新鲜度 */
            account(work[n].result, lane);
            hit = work[n].result;          /* 返回判定码（JR_PERFECT..） */
            break;
        }
    }
    return hit;
}

/* 每帧调用：超时未打 → MISS */
void Judge_TimeoutMisses(uint32_t songTimeMs)
{
    int32_t t = (int32_t)songTimeMs + JDG_OFFSET_MS;
    while (head < workCount && work[head].judged) head++;
    while (head < workCount && (t - (int32_t)work[head].t_ms) > JDG_WIN_GOOD)
    {
        work[head].judged = 1;
        work[head].result = JR_MISS;
        lastResult   = JR_MISS;
        lastResultMs = songTimeMs;
        account(JR_MISS, work[head].lane);
        head++;
    }
}

uint16_t Judge_CountOf(uint8_t result) { return counts[result]; }
uint32_t Judge_Score(void)     { return score; }
uint16_t Judge_Combo(void)     { return combo; }
uint16_t Judge_MaxCombo(void)  { return maxCombo; }
uint8_t  Judge_FullCombo(void) { return (uint8_t)(!anyBreak && workCount); }

uint32_t Judge_LastHitNoteMs(void) { return lastHitNoteMs; }

/* 谱面最后一个音符的时刻（ms），供结束条件计算 */
uint32_t Judge_LastNoteMs(void) { return lastNoteMs; }

/* 该轨长条的结束时刻（曲目 ms）。0 或已经过去 = 没有正在显示的长条尾巴。 */
uint32_t Judge_HoldTailMs(uint8_t lane)
{
    return (lane < 5u) ? holdTailMs[lane] : 0;
}

uint8_t Judge_HoldActive(uint8_t lane)
{
    return (lane < 5u) ? holdActive[lane] : 0;
}

/* 提前松手：按松手时刻与尾巴时刻之差分级；早于 GOOD 窗口算断掉（MISS） */
uint8_t Judge_HoldRelease(uint8_t lane, uint32_t releaseSongT)
{
    int32_t dt;
    uint8_t res;

    if (lane >= 5u || !holdActive[lane]) return JR_NONE;

    dt = (int32_t)releaseSongT + JDG_OFFSET_MS - (int32_t)holdTailMs[lane];
    holdActive[lane] = 0;                       /* 一次长条只尾判一次 */

    if (dt < -JDG_WIN_GOOD) res = JR_MISS;      /* 提前太多：长条断掉 */
    else                    res = grade_of(dt < 0 ? -dt : dt);

    lastResult   = res;
    lastResultMs = releaseSongT;
    account(res, lane);
    return res;
}

/* 到尾检查：仍按着 = 按到底（PERFECT）；已松手但没走 release 路径 = MISS。
   返回本次判定过的轨掩码，供上层发遥测。 */
uint8_t Judge_HoldTick(uint32_t songT, uint8_t pressedMask)
{
    uint8_t lane, mask = 0;
    int32_t t = (int32_t)songT + JDG_OFFSET_MS;

    for (lane = 0; lane < 5u; lane++)
    {
        if (!holdActive[lane]) continue;
        if (t < (int32_t)holdTailMs[lane]) continue;   /* 还没到尾 */

        holdActive[lane] = 0;
        if (pressedMask & (1u << lane))
        {
            lastResult = JR_PERFECT;
            account(JR_PERFECT, lane);
        }
        else
        {
            lastResult = JR_MISS;
            account(JR_MISS, lane);
        }
        lastResultMs = songT;
        mask |= (uint8_t)(1u << lane);
    }
    return mask;
}

/* 最近一局命中音符的 dt 均值（ms）。设置界面用它标定 JDG_OFFSET_MS：
   把 OFF 调到 -dtAvg 即可让均值归零。 */
int32_t Judge_DtMeanMs(void) { return dtN ? (dtSum / (int32_t)dtN) : 0; }

/* 最近一次判定与发生时刻（曲目 ms），供渲染层做实时显示 */
uint8_t  Judge_LastResult(void)   { return lastResult; }
uint32_t Judge_LastResultMs(void) { return lastResultMs; }
uint16_t Judge_DtCount(void) { return dtN; }

const Note *Judge_Notes(void)  { return work; }
uint16_t Judge_NoteCount(void) { return workCount; }
uint16_t Judge_Head(void)      { return head; }
