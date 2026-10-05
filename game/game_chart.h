//============================================================================//
//  game_chart.h - 谱面定义
//  v1 方案 A：手写 C 数组直接编译进工程（project.md §10.2）。
//  Note 8 字节； judging 状态存 RAM 镜像（Flash 只读）。
//============================================================================//
#ifndef GAME_CHART_H
#define GAME_CHART_H

#include <stdint.h>

#define CH_MAX_NOTES   160            /* v1: 单首谱面音符上限（RAM 镜像 8 B/个 = 1.25 KB） */
#define CH_LANES       5

typedef struct {
    uint32_t t_ms;      /* 到达判定线的时刻（相对歌曲开始） */
    uint8_t  lane;      /* 0..4 */
    uint8_t  speed;     /* 保留：落速倍率（v1 恒 1） */
    uint8_t  judged;    /* 运行时状态：0=未判 1=已判 */
    uint8_t  result;    /* 运行时状态：JudgeResult */
    uint8_t  pad[2];
} Note;                 /* 8 字节，自然对齐 */

typedef struct {
    const char   *title;
    uint32_t      length_ms;
    uint16_t      noteCount;
    uint16_t      bpm;          /* 打谱参考；运行时不需 */
    const Note   *notes;        /* Flash 中的只读数组 */
} Chart;

/* 已判定结果（与判定窗口一致） */
enum { JR_NONE = 0, JR_MISS = 1, JR_GOOD = 2, JR_GREAT = 3, JR_PERFECT = 4, JR_WRONG = 5 };

/* 内置谱面表（P4 用 3 首不同密度的测试谱；由 tools/mkchart.py 生成） */
extern const Chart  CHART_bezier_0;      /* 简单：120 BPM 单音 8 分 */
extern const Chart  CHART_bezier_1;      /* 中等：加双押/连音 */
extern const Chart  CHART_bezier_2;      /* 密集：考验同轨连打 */
extern const Chart *const CHART_list[3];
#define CHART_COUNT  3

#endif /* GAME_CHART_H */
