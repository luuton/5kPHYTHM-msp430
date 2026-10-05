//============================================================================//
//  game_render.h - 渲染层：从 Judge/Play 拉数据 → 画到影子 → Present
//============================================================================//
#ifndef GAME_RENDER_H
#define GAME_RENDER_H

#include <stdint.h>

/* 屏幕几何 */
#define REN_LANE_W          20     /* 每轨宽 */
#define REN_LANE_X(lane)    ((lane) * REN_LANE_W)   /* lane0.x=0 */
#define REN_NOTE_W          16     /* 音符宽 */
#define REN_NOTE_H          4      /* 音符高 */
#define REN_STATUS_PAGES    1      /* 顶部状态栏高度（页） */
#define REN_JUDGE_Y         55     /* 判定线 y */
#define REN_PLAY_TOP_Y      8      /* 下落区顶（避开状态栏） */

extern void Render_Init(void);
extern void Render_Frame(uint8_t lanePressedBits);    /* 每帧主循环调用 */
extern void Render_DemoIdle(void);                    /* 主菜单背景 */

/* 调试画面：5 路 delta 柱状图 + 阈值（P3 标定用） */
extern void Render_Calibration(const uint16_t *delta, const uint16_t *thr);

/* 主菜单：曲目列表 + 光标 + 简单提示 */
extern void Render_MainMenu(uint8_t selected);

/* READY 3-2-1（remainMs 为剩余毫秒） */
extern void Render_Ready(uint32_t remainMs);

/* 暂停页 */
extern void Render_Paused(void);

/* 结算页 */
extern void Render_Result(void);

#endif /* GAME_RENDER_H */
