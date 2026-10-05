//============================================================================//
//  game_touch.h - 触摸输入层
//  基于 CTS 库 TI_CAPT_Custom()，独立 Sensor（baseOffset=5），
//  每路保留原始 delta 做逐路判定 → 天然支持多键同按（和弦）。
//============================================================================//
#ifndef GAME_TOUCH_H
#define GAME_TOUCH_H

#include <stdint.h>

extern const struct Sensor keys5;

/* 运行时可改的逐路阈值（HAL element.threshold 是 const，不能改） */
extern uint16_t Touch_Thresholds[5];

/* 初始化：建基线。必须在 Board_init() 之后调用；loops=建议 8 */
extern void Touch_Init(uint8_t loops);

/* 只取每路原始 delta（标定画面用） */
extern void Touch_RawScan(uint16_t *delta);

/* 扫描一次（约 0.8-4 ms）：
   delta[5]   = 每路原始 delta
   pressed[5] = 经主导键判据后的按下状态
   返回按下路位图 bit0..bit4 = lane0..lane4 */
extern uint8_t Touch_Scan(uint16_t *delta, uint8_t *pressed);

/* 长按 3 s 基线保护（预留） */
extern void Touch_BaselineGuard(const uint16_t *delta);

#endif /* GAME_TOUCH_H */
