//============================================================================//
//  game_main.h - 5K 下落式节奏游戏总装（外接主循环框架）
//============================================================================//
#ifndef GAME_MAIN_H
#define GAME_MAIN_H

#include <stdint.h>

/* 游戏测试/演示入口：调用后不再返回（内含主循环）。
   在 LAB1main.c 的 menu 处替换原 lab1() 调用。 */
extern void Game5K_Run(void);

/* 时基演示（P0/E0.1 用）：8×8 方块每帧右移 1 px，S1 切 60/30/15 fps */
extern void Demo_LcdMotion(void);

/* 触摸标定画面（P0/E0.2、P3 用） */
extern void Demo_Calib(void);

/* CTS 扫描耗时测量（P0/E0.2 用）：把 min/avg/max tick 打到 LCD */
extern void Demo_ScanTiming(void);

#endif /* GAME_MAIN_H */
