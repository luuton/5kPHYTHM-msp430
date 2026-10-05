//============================================================================//
//  game_menu.h - 菜单与按键（S1/S2 轮询消抖）
//============================================================================//
#ifndef GAME_MENU_H
#define GAME_MENU_H

#include <stdint.h>

/* 一次性事件（每帧清零） */
typedef struct {
    uint8_t s1Short;
    uint8_t s2Short;
    uint8_t s1Long;
    uint8_t s2Long;
} MenuEvents;

/* 初始化（按键 GPIO 已由 Buttons_init 完成；轮询不需要中断） */
extern void Menu_Init(void);

/* 每帧调用一次：读 P1.7/P2.2 → 消抖 → 长短按 */
extern void Menu_ScanEvents(MenuEvents *ev, uint32_t nowMs);

/* 主菜单状态机（含光标显示/进入游戏/返回） */
extern void Menu_Update(const MenuEvents *ev, uint32_t nowMs);

/* 当前选中的曲目 index（Play_Start 用） */
extern uint8_t Menu_SelectedChart(void);

/* 设置界面的光标项（0..3），供渲染层使用 */
extern uint8_t Menu_SettingsSel(void);

/* 菜单状态 */
extern uint8_t Menu_Screen(void);   /* 0=logo 1=主菜单 2=游戏中 3=暂停 4=结算 5=设置 */

#endif /* GAME_MENU_H */
