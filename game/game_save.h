//============================================================================//
//  game_save.h - 设置持久化（存 INFO D @0x1800）
//
//  为什么要持久化：设置界面调好的判定偏移等参数原本只在 RAM，断电即丢，
//  每次上电都要重调。存进 INFO Flash 后可掉电保持。
//
//  为什么用 INFO D：INFO C(0x1880) 被 lab1.c 的对比度/背光占用，
//  INFO D(0x1800) 全段未被使用，且它是独立段，擦写不会波及别的数据。
//  （lab1.c 的 WriteFlashSettings 只回写 INFO C 的前 16 字，若把游戏设置
//   塞进 INFO C 会被它擦掉，所以必须分开。）
//============================================================================//
#ifndef GAME_SAVE_H
#define GAME_SAVE_H

#include <stdint.h>

/* 上电调用：INFO D 中有有效记录则读入并应用；否则沿用源码里的默认值。
   必须在 Telem_Version 之前调用，这样开机横幅报的就是实际生效的参数。 */
extern void Save_LoadSettings(void);

/* 把当前设置写回 INFO D。内部会关中断约数毫秒（擦除+写入）。
   调用点应避开实时路径 —— 目前只在退出设置界面时调用。 */
extern void Save_StoreSettings(void);

#endif /* GAME_SAVE_H */
