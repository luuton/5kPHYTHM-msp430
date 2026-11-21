//============================================================================//
//  game_touch.c - 触摸输入层实现
//
//  依赖 CTS 库（CTS_Layer / CTS_HAL / structure）。新建 keys5 Sensor：
//    · halDefinition = fRO_COMPB_TA1_SW（与 slider 相同的测量方法）
//    · baseOffset    = 5（基线存到 baseCnt[5..9]，与 slider 的 [0..4] 隔离）
//    · cbpdBits 等 P1.6/CBPD 配置与 slider 完全相同（硬件同一批焊盘）
//  这样 C 题滑条用法与本游戏的单键用法互不污染。
//
//  注意：TOTAL_NUMBER_OF_ELEMENTS 必须从 5 扩到 10（structure.h），
//  否则 baseCnt[5..9] 越界。
//
//  判定完全由本模块计算（不依赖 CTS 库的 EVNT/ Dominant_Element）：
//    1. 逐路与自己的 LANE_THR 比较
//    2. 主导键判据：delta[i] ≥ 邻居 delta 的 2/3 → 抑制串扰
//============================================================================//
#include <stdint.h>
#include "msp430.h"
#include "CTS_Layer.h"
#include "CTS_HAL.h"        /* TI_CTS_fRO_COMPB_TA1_SW_HAL：单路重标定用 */
#include "game_touch.h"

/* CTS 库把基线数组放在 CTS_Layer.c 里，没有对外声明，这里补上。
   baseCnt[0..4] 是滑条，keys5 用 baseCnt[5..9]（baseOffset = 5）。 */
extern uint16_t baseCnt[];

extern const struct Element element0, element1, element2, element3, element4;

const struct Sensor keys5 = {
    .halDefinition  = fRO_COMPB_TA1_SW,
    .numElements    = 5,
    .baseOffset     = 5,                /* baseCnt[5..9] */
    .arrayPtr[0]    = &element0,
    .arrayPtr[1]    = &element1,
    .arrayPtr[2]    = &element2,
    .arrayPtr[3]    = &element3,
    .arrayPtr[4]    = &element4,
    .cboutTAxDirRegister = (unsigned char *)&P1DIR,
    .cboutTAxSelRegister = (unsigned char *)&P1SEL,
    .cboutTAxBits   = BIT6,             /* CBOUT -> P1.6 -> TA1CLK */
    .cbpdBits       = 0x001F,           /* CBPD0..4 */
    .accumulationCycles = 50            /* E0.2 超时则改 20-25 并重标定 */
};

/* 主导键判据的容差：本路 delta 允许比邻居低这么多（0 = 要求严格局部极大）。
   实测数据（test.txt）显示相邻焊盘串扰明显，先用 0（严格）。 */
#define TOUCH_DOM_SLACK   0

/* 初始阈值：⚠ 占位值，未经实测标定。用 Demo_Calib 读串口
   （Telem_Calib 输出当前值/峰值）后按"按下幅度的 40~50%"重填。
   实测症状：lane0 触发 53 次而谱面只有 22 次，说明阈值偏低。 */
uint16_t Touch_Thresholds[5] = { 112, 175, 153, 225, 180 };

/* 灵敏度倍率（%），见 game_touch.h */
uint16_t Touch_SensPct = 100;

/* 迟滞：一旦某路被判为按下，必须跌到阈值的 60% 以下才允许再次触发。
   实测 test.txt：同一路在 0~30 ms 内重复上报 27 次（抖动/阈值悬停），
   而人手指的一次按压不会在 30 ms 内跌到 60% 以下。
   只做迟滞、不做"连续 2 帧确认"：后者会给判定增加约 16 ms 固定延迟，
   对目押游戏是实打实的负收益。 */
static uint8_t pressedMask = 0;

/* ---- 卡键自愈 ----
   某一路连续被判为按下超过 STUCK_SCANS 次扫描（一次约 1.3 ms，合 1.5 s 以上），
   且幅度没有明显超过阈值（< 2x 阈值），就判定它是在阈值边缘"悬停"：
   重新标定这一路的基线并立即释放。
   为什么需要：实测 test.txt 里 lane0 整首歌只被真正按过 1 次，却上报了 42 次，
   间隔中位数 952 ms —— 基线漂移让 delta 长期贴着阈值，形成假按住。
   为什么加"幅度 < 2x 阈值"：玩家刻意按住时 delta 通常远大于阈值，
   这样不会把真实的长按误判成卡键。 */
#define STUCK_SCANS  1200u
static uint16_t stuckCnt[5] = {0, 0, 0, 0, 0};

/* ---- 测量失效（尖峰）过滤 ----
   CTS 的 fRO 方法本质是"数软件循环次数"：
       while(!(TA1CTL & TAIFG)) { j++; }
   只要这期间进了一次中断（USB、定时器等），j 就会明显偏小，
   于是 delta = 基线 - 计数被抬到几百甚至几千。实测（test.txt 的 M/X 行）
   这些假值在 1200~3300，而真人按压的峰值只有 300~600，量级完全不同。
   处理：发现任一路超过 TOUCH_DELTA_SPIKE 就整组重测一次；仍超限的那一路
   直接判为"本帧无效"，保持上一状态、不产生边沿。
   为什么不用"测量期间关中断"：一次整组测量约 1.3 ms，关中断会显著拉长
   USB 中断响应，枚举和 CDC 都会受影响；过滤的代价只有偶尔多测一次。 */
#define TOUCH_DELTA_SPIKE 900u

void Touch_Init(uint8_t loops)
{
    uint8_t i;
    pressedMask = 0;
    for (i = 0; i < 5; i++) stuckCnt[i] = 0;
    TI_CAPT_Init_Baseline(&keys5);
    TI_CAPT_Update_Baseline(&keys5, loops);
}

void Touch_RawScan(uint16_t *delta)
{
    TI_CAPT_Custom(&keys5, delta);
}

uint8_t Touch_Scan(uint16_t *delta, uint8_t *pressed)
{
    uint8_t lane, hit = 0;
    uint8_t bad;
    uint8_t raw[5];        /* 各自过阈值的结果（还没做相邻抑制） */

    Touch_RawScan(delta);

    /* 有尖峰就整组重测一次（大多数情况下第二次是干净的） */
    bad = 0;
    for (lane = 0; lane < 5; lane++)
        if (delta[lane] > TOUCH_DELTA_SPIKE) bad = 1;
    if (bad) Touch_RawScan(delta);

    for (lane = 0; lane < 5; lane++)
    {
        uint16_t d   = delta[lane];

        /* 重测后仍然超限 → 这一次测量不可信：保持上一状态，不产生边沿 */
        if (d > TOUCH_DELTA_SPIKE)
        {
            uint8_t was = (uint8_t)((pressedMask >> lane) & 1u);
            pressed[lane] = was;
            if (was) hit |= (uint8_t)(1u << lane);
            delta[lane] = 0;
            continue;
        }

        uint16_t thr = (uint16_t)(((uint32_t)Touch_Thresholds[lane]
                                   * (uint32_t)Touch_SensPct) / 100u);
        /* 已按下时用 60% 门限（迟滞），未按下用完整门限 */
        uint16_t lim = (pressedMask & (1u << lane)) ? (uint16_t)((thr * 3u) / 5u) : thr;
        uint8_t  ok  = (uint8_t)(d >= lim);

        /* --- 卡键自愈（见文件上方说明） --- */
        if (ok)
        {
            if (stuckCnt[lane] < 0xFFFFu) stuckCnt[lane]++;
            if (stuckCnt[lane] > STUCK_SCANS && (uint32_t)d < (uint32_t)thr * 2u)
            {
                uint16_t m[5];
                TI_CTS_fRO_COMPB_TA1_SW_HAL(&keys5, m);      /* 重测一次原始值 */
                baseCnt[keys5.baseOffset + lane] = m[lane];  /* 只改这一路 */
                stuckCnt[lane] = 0;
                ok = 0;                                      /* 立即释放 */
                delta[lane] = 0;
            }
        }
        else stuckCnt[lane] = 0;

        raw[lane] = ok;

        if (ok)
        {
            /* 主导键判据（局部极大）：本路 delta 必须 ≥ 左右邻居。
               为什么不用"≥邻居×2/3"：串扰一旦超过邻居的 2/3 就能通过，
               实测相邻焊盘会互相误触发（打 lane1 时 lane0 跟着报）。
               要求局部极大后：
                 · 真按 lane1(200) + lane0 串扰(150) → lane0 被拒 ✓
                 · 非相邻和弦 → 各自邻居都弱 → 都通过 ✓
                 · 相邻等强同按(200,200) → 相等仍通过 ✓
               若实测串扰比例很高（>95%）导致真键被邻居压掉，把
               TOUCH_DOM_SLACK 调大（允许本路比邻居低这么多仍算有效）。 */
            /* ⚠ 只和"自己也过阈值"的邻居比较。
               旧写法是拿本路和邻居的原始 delta 比，哪怕邻居根本没按（例如
               lane0 阈值 112、邻居 lane1 阈值 175），lane1 的底噪 125 也会把
               lane0 的真按压压掉 —— 那会造成漏按。 */
            /* 相等时按"小轨号优先"裁决：两路读数值完全相同时（手指压在两个
               焊盘中间、或串扰把它们拉成同一个数），旧写法是两边都通过，
               于是按一个键会同时点亮相邻轨的 LED。现在相等时只有小轨号胜出。 */
            if (lane > 0 && raw[lane-1] &&
                ((uint32_t)d + TOUCH_DOM_SLACK <= delta[lane-1])) ok = 0;
            if (lane < 4 && raw[lane+1] &&
                ((uint32_t)d + TOUCH_DOM_SLACK <  delta[lane+1])) ok = 0;
        }
        pressed[lane] = ok;
        if (ok) hit |= (uint8_t)(1u << lane);
    }
    pressedMask = hit;
    return hit;
}

/* 长按 3 s 的基线保护（P8 打磨阶段按实测情况实现） */
void Touch_BaselineGuard(const uint16_t *delta)
{
    (void)delta;
}
