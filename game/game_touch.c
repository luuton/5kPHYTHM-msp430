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
#include "game_touch.h"

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

void Touch_Init(uint8_t loops)
{
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
    Touch_RawScan(delta);
    for (lane = 0; lane < 5; lane++)
    {
        uint16_t d = delta[lane];
        uint8_t  ok = (uint8_t)(d >= Touch_Thresholds[lane]);
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
            if (lane > 0 && (uint32_t)d + TOUCH_DOM_SLACK < delta[lane-1]) ok = 0;
            if (lane < 4 && (uint32_t)d + TOUCH_DOM_SLACK < delta[lane+1]) ok = 0;
        }
        pressed[lane] = ok;
        if (ok) hit |= (uint8_t)(1u << lane);
    }
    return hit;
}

/* 长按 3 s 的基线保护（P8 打磨阶段按实测情况实现） */
void Touch_BaselineGuard(const uint16_t *delta)
{
    (void)delta;
}
