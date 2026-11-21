//============================================================================//
//  game_telem.h - 串口遥测（USCI_A1，P4.4/P4.5，115200-8-N-1）
//
//  重要性：判定调试窗口。回传三类数据：
//    1. 击打事件行：tap 时刻 / 音符时刻 / dt / 判定（人类可读，PC 串口助手直接看）
//    2. 帧统计：每秒一帧汇总（帧最长耗时 / CTS 全扫 min/avg/max）
//    3. 谱面结束：判定统计
//
//  发送是阻塞的（UCA1 TX FIFO 无缓冲 FIFO 只有 1 级）——一行 ~30 字节 @115200
//  ≈ 2.6 ms，只在击打瞬间（扫描窗口内）发送，帧预算安全；主循环每秒汇总行
//  4 行 ~120 B ≈ 10 ms，分摊在帧内可接受。TX 中途其余发送会被 putChar 忙等——
//  所以遥测行要短、频率要低。
//============================================================================//
#ifndef GAME_TELEM_H
#define GAME_TELEM_H

/* 遥测输出后端：
     1 = USB CDC（推荐，非阻塞，一根线搞定供电+通信）
     0 = USCI_A1 UART（P4.4/P4.5，115200，需外接 USB-TTL）
   两者共用同一套 ASCII 行格式；改为 0 可在 USB 出问题时快速回退。 */
#ifndef TELEM_BACKEND_USB
#define TELEM_BACKEND_USB  1   /* USB CDC（Windows 上是一个 COM 口，波特率无所谓）。
                                   枚举失败时的回退：改成 0 即走 UART（P4.4/P4.5, 115200）。
                                   枚举死锁的根因已定位并修好（开机假 suspend 后 USBIE
                                   被收窄成 RESRIE，见 Usb_Poll 里的软件兜底），r40 起默认用 USB。 */
#endif

#include <stdint.h>

extern void Telem_Init(void);            /* UCA1 115200 @ SMCLK25M */

/* 击打事件入队（非阻塞，可在扫描窗口内调用）。
   noteMs = 命中的音符时刻；未命中传 0xFFFFFFFF */
extern void Telem_Tap(uint8_t lane, uint32_t tapMs, uint32_t noteMs, uint8_t result,
                      const uint16_t *delta);

/* 版本横幅：开机打一行，记录本固件的全部可调参数。
   为什么需要它：多次分析串口日志时无法判断"这份 log 烧的是哪版固件"，
   导致把建模仿真建立在错误的基线上（本工程真实踩过）。
   每次改动后请把 tag 往上带一位。 */
extern void Telem_Version(const char *tag, uint16_t speedPx, int16_t alignPx,
                          int32_t winPerfect, int32_t winGreat, int32_t winGood,
                          const uint16_t *thr5);

/* 把队列里的击打行统一发出（扫描窗口外调用，会阻塞到发完） */
extern void Telem_Flush(void);

/* 帧耗时（每秒汇总一次）：最长帧 ms、CTS 扫描 min/avg/max us */
/* 每秒汇总一行 F：
     F <worst_ms> <scanMin> <scanAvg> <scanMax> <body_ms> <render_us>
   worst  整帧最长（含等待）
   body   帧体最长（从帧开始到进入等待之前）—— 接近 worst 说明是干活慢，
          远小于 worst 说明是等待循环/计时出了问题
   render 渲染阶段最长 us */
extern void Telem_FrameStats(uint16_t worstFrameMs, uint16_t bodyMaxMs,
                             uint16_t renderMaxUs,
                             uint16_t scanMinUs, uint16_t scanAvgUs, uint16_t scanMaxUs,
                             uint16_t drawMaxUs, uint16_t presentMaxUs);

/* 时钟对照行：游戏毫秒计时 vs RTC 秒，用来验证时基本身准不准 */
extern void Telem_Clock(uint32_t gameSec, uint8_t rtcSec);

/* 结算统计 */
extern void Telem_Result(uint16_t pf, uint16_t gr, uint16_t gd, uint16_t ms,
                         uint32_t score, uint16_t maxCombo);

/* 标定行：当前 delta / 峰值 / 当前阈值（Demo_Calib 用，比读 LCD 准） */
extern void Telem_Calib(const uint16_t *delta, const uint16_t *peak, const uint16_t *thr);

/* 遥测总开关（P3 标定时可关，避免串口耗时干扰扫描） */
/* USB 诊断快照行（诊断页显示期间每秒一次） */
/* 每秒一次：各路 delta 的最小/最大值（不管是否判为按下）。
   这是判断"卡键"还是"阈值偏低"的直接依据：
     · 卡键：某路 min 也接近阈值（整秒都没掉下去）
     · 噪声：min 很低、max 周期性冲到阈值附近 */
extern void Telem_DeltaRange(const uint16_t *dmin, const uint16_t *dmax);

/* 长帧标记（帧耗时 > 500 ms 时自动输出） */
/* 演奏中每秒一行：状态 / 曲目时间 / 已判音符数 / 总音符数。
   用来定位"铺面突然没了"：head 追上 count 就说明音符已经判完，
   若此时 songMs 远小于谱面长度，就是时间跳变或载入被截断。 */
extern void Telem_Play(uint8_t state, uint32_t songMs,
                       uint16_t head, uint16_t count);

extern void Telem_Stall(uint32_t frameMs, uint32_t nowMs);


extern void Telem_Enable(uint8_t on);

#endif /* GAME_TELEM_H */
