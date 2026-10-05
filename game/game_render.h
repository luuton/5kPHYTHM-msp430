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

/* 音符在判定线处的对齐偏移（px，正值把音符整体上移；dt=0 时音符相对判定线 55 的位置：
   0=顶边贴线 / 2=中心贴线 / 3=底边贴线）。
   ⚠ 实测结论（logC aln=0 dt−51 vs log3 aln=3 dt−44）：3px 只让 dt 移动 7ms，
   说明玩家按节奏预判而非按屏幕位置瞄准，**视觉对齐对判定时机影响很小**。
   该项保留为可调，但补偿打点偏置应优先用 JDG_OFFSET_MS。
   已从宏改为变量，可在设置界面实时调整，并由遥测 V 行输出。 */
extern int16_t Render_NoteAlignPx;


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

/* 设置界面（sel = 当前光标项 0..3） */
extern void Render_Settings(uint8_t sel);

/* 结算页 */
extern void Render_Result(void);

#endif /* GAME_RENDER_H */
