//============================================================================//
//  game_rtlcd.h - 影子缓冲 + 批量 flush 的 LCD 图层
//
//  原理：DOGS102x6 的 HAL（Dogs102x6_*）是"写穿式"的，每个绘制原语都立即
//  推屏。本模块绕开它，直接操作 HAL 暴露的显存数组 dogs102x6Memory[]（
//  格式：page-major，+'2' 头部 2 字节），画完一帧后用 flushPage 批量推屏。
//
//  这样 5 轨全量重绘 = 8 页 × 102 B = 816 B，即 8 次 writeData(102)，
//  实测约 2.7 ms @25 MHz（见 project.md §5.3），完全在 60 fps 帧预算内。
//
//  用法：
//      RTLCD_Init();                 /* 清影子 + 立即全屏 flush 一次 */
//      RTLCD_Clear();                /* 影子清零（不推屏） */
//      RTLCD_Pixel(x, y, 1);         /* 坐标系：x 0..101 左→右，y 0..63 上→下 */
//      RTLCD_FillRect(x1, y1, x2, y2, 1);
//      RTLCD_Char(x, y, 'A', 0);     /* 6x8 字体，x/y 为像素坐标 */
//      RTLCD_String(x, y, "HI", 0);
//      ... 决定一帧内容完毕 ...
//      RTLCD_Present();              /* 把 8 页影子批量推屏（约 2.7 ms） */
//
//  与 HAL 的混用规则：一帧内不要混用 Dogs102x6_*Direct* 原语和本模块；
//  本模块的 Present 会覆盖整个屏幕。
//============================================================================//
#ifndef GAME_RTLCD_H
#define GAME_RTLCD_H

#include <stdint.h>

#define RTLCD_W   102
#define RTLCD_H   64
#define RTLCD_PAGES 8

extern void RTLCD_Init(void);
extern void RTLCD_Clear(void);                 /* 影子清零，不推屏 */
extern void RTLCD_Present(void);               /* 整屏 8 页批量推屏 */
extern void RTLCD_PresentPages(uint8_t mask);  /* 只推 1 的页（位0=page0..位7=page7） */

/* 只改影子 */
extern void RTLCD_Pixel(uint8_t x, uint8_t y, uint8_t on);
extern void RTLCD_FillRect(uint8_t x1, uint8_t y1, uint8_t x2, uint8_t y2, uint8_t on);
extern void RTLCD_HLine(uint8_t x1, uint8_t x2, uint8_t y, uint8_t on);
extern uint8_t RTLCD_Char(uint8_t x, uint8_t y, char ch, uint8_t inv);   /* 返回占用宽 6 */
extern void RTLCD_String(uint8_t x, uint8_t y, const char *s, uint8_t inv);
/* 用 HAL 里的 6x8 字体画一个大号数字/字符的 2x 放大版（12x16），用于结算页分数 */
extern void RTLCD_Char2x(uint8_t x, uint8_t y, char ch, uint8_t inv);
extern void RTLCD_String2x(uint8_t x, uint8_t y, const char *s, uint8_t inv);

#endif /* GAME_RTLCD_H */
