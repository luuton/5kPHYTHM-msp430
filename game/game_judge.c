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
//  RAM 镜像策略：谱面在 Flash 只读，运行前拷一份到 RAM（Note 8 B × 160 = 1280 B，
//  RAM 有 6.7 KB 空闲），judged/result 直接写在镜像里。
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
static uint8_t  anyBreak;               /* 是否有过 MISS/WRONG（full combo 用） */
static uint32_t lastHitNoteMs = 0xFFFFFFFFu;   /* 遥测：最近命中的音符时刻 */
static int32_t  dtSum = 0;                     /* dt 累计（含符号），用于设置界面标定 */
static uint16_t dtN   = 0;

const Chart *Judge_Chart(void) { return cur; }

void Judge_LoadChart(const Chart *c)
{
    uint16_t i;
    cur = c;
    workCount = (c->noteCount > CH_MAX_NOTES) ? CH_MAX_NOTES : c->noteCount;
    for (i = 0; i < workCount; i++)
    {
        work[i] = c->notes[i];          /* struct 拷贝（8 B） */
        work[i].judged = 0;
        work[i].result = JR_NONE;
    }
    head = 0;
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
    combo = maxCombo = 0;
    score = 0;
    anyBreak = 0;
    lastHitNoteMs = 0xFFFFFFFFu;
    dtSum = 0; dtN = 0;
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
            lastHitNoteMs = work[n].t_ms;
            dtSum += dt;                       /* 记录带符号的 dt，供均值标定 */
            dtN++;
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

/* 最近一局命中音符的 dt 均值（ms）。设置界面用它标定 JDG_OFFSET_MS：
   把 OFF 调到 -dtAvg 即可让均值归零。 */
int32_t Judge_DtMeanMs(void) { return dtN ? (dtSum / (int32_t)dtN) : 0; }
uint16_t Judge_DtCount(void) { return dtN; }

const Note *Judge_Notes(void)  { return work; }
uint16_t Judge_NoteCount(void) { return workCount; }
uint16_t Judge_Head(void)      { return head; }
