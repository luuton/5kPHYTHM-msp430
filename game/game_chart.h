//============================================================================//
//  game_chart.h - 谱面定义
//  v1 方案 A：手写 C 数组直接编译进工程（project.md §10.2）。
//  Note 8 字节； judging 状态存 RAM 镜像（Flash 只读）。
//============================================================================//
#ifndef GAME_CHART_H
#define GAME_CHART_H

#include <stdint.h>

/* ⚠ 必须覆盖最长的谱面：easy=88 / normal=250 / hard=412。
   原来写 160 会把 normal 截掉 36%、hard 截掉 61% —— 表现就是
"打着打着铺面没了"（后面根本没有音符可放，长时间只有空轨道）。
   音符结构已压缩到 4 B/个，416 个 = 1664 B，比原来的 160x8B 只多 384 B。 */
#define CH_MAX_NOTES   416
#define CH_LANES       5

typedef struct {
    uint16_t t_ms;      /* 到达判定线的时刻（ms）。谱面最长 50 s，uint16 足够 */
    uint8_t  lane;      /* 0..4 */
    uint8_t  speed  : 2;/* 保留：落速倍率（v1 恒 1） */
    uint8_t  judged : 1;/* 运行时状态：0=未判 1=已判 */
    uint8_t  result : 3;/* 运行时状态：JudgeResult（0..5） */
    uint16_t hold_ms;   /* 长条长度（ms）；0 = 普通单点音符。
                           注意：长条只有"头判"，尾端不判定 —— 到时间尾巴
                           自然消失，玩家松手不受惩罚（需求如此）。
                           放在结构末尾是为了让谱面里已有的 5 个初始化值
                           自动补 0，不必改动所有行。 */
} Note;                 /* 6 字节 */

/* 编译期断言：结构一旦被改回更大的布局，这里立刻报错，避免 RAM 无声膨胀 */
typedef char _note_size_check[(sizeof(Note) == 6) ? 1 : -1];

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
