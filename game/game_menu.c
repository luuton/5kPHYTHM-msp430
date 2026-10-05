//============================================================================//
//  game_menu.c - 菜单状态机与 S1/S2 轮询
//
//  轮询替代 HAL_Buttons ISR（project.md §6.2 的三分原因：ISR 不打时间戳、
// buttonsPressed 是赋值不是或、消抖是死代码）。菜单键精度要求 ±20 ms，帧级
//  轮询足够。
//
//  状态：
//    M_LOGO     开机 logo，任意键进主菜单
//    M_MAIN     主菜单（曲目列表 + 设置项）S1 切换，S2 确认
//    M_PLAY     游戏中（S2 短按 = 暂停切换；S1 长按 = 回主菜单）
//    M_PAUSE    暂停菜单（S2 = 继续；S1 短按 = 重开；S1 长按 = 退出）
//    M_RESULT   结算（S2 短按 = 重玩；S1 短按 = 主菜单）
//============================================================================//
#include <stdint.h>
#include "msp430.h"
#include "HAL_Buttons.h"
#include "HAL_Wheel.h"
#include "game_menu.h"
#include "game_play.h"
#include "game_judge.h"
#include "game_chart.h"
#include "game_render.h"

enum { M_LOGO = 0, M_MAIN = 1, M_PLAY = 2, M_PAUSE = 3, M_RESULT = 4 };

#define MENU_LONG_MS   600u
#define MENU_DB_FRAMES 3u          /* 3 帧连续同状态才算 */
#define RESULT_WAIT_MS 800u       /* 结算至少停 0.8 s 防误触 */

static uint8_t  screen = M_LOGO;
static uint8_t  selected = 0;
static uint16_t s1Hist, s2Hist;    /* 消抖历史 */
static uint8_t  s1Down, s2Down;    /* 消抖后的稳定按下状态 */
static uint32_t s1DownAtMs, s2DownAtMs;
static uint32_t resultShownAt;
static uint16_t wheelLast = 0xFFFF;      /* 上次滚轮值（0xFFFF=未初始化） */

uint8_t Menu_Screen(void)         { return screen; }
uint8_t Menu_SelectedChart(void)  { return selected; }



void Menu_Init(void)
{
    s1Hist = s2Hist = 0;
    s1Down = s2Down = 0;
    screen = M_LOGO;
    selected = 0;
}

void Menu_ScanEvents(MenuEvents *ev, uint32_t nowMs)
{
    /* BUTTON_S1/S2 都是 PA(P1+P2) 的 16-bit 掩码：S1=0x0080(P1.7)、S2=0x0400(P2.2)。
       P1IN/P2IN 只有 8 位；P2IN & 0x0400 恒 0（这就是之前 S2 失灵的根因）。
       用 PAIN 一次读 16 位再掩码。 */
    uint16_t pa = PAIN;
    uint8_t s1raw = (pa & BUTTON_S1) ? 0u : 1u;    /* 低电平有效 */
    uint8_t s2raw = (pa & BUTTON_S2) ? 0u : 1u;

    ev->s1Short = ev->s2Short = ev->s1Long = ev->s2Long = 0;

    /* 消抖：连续 MENU_DB_FRAMES 帧相同才算稳定电平（3 帧 ≈ 50 ms） */
    s1Hist = (uint16_t)((s1Hist << 1) | s1raw);
    s2Hist = (uint16_t)((s2Hist << 1) | s2raw);

    {
        uint8_t s1new = (uint8_t)((s1Hist & ((1u << MENU_DB_FRAMES) - 1u)) == ((1u << MENU_DB_FRAMES) - 1u));
        uint8_t s2new = (uint8_t)((s2Hist & ((1u << MENU_DB_FRAMES) - 1u)) == ((1u << MENU_DB_FRAMES) - 1u));

        /* 稳定电平上升沿 → 长按计时起点；下降沿 → 结算长短按 */
        if (s1new && !s1Down) { s1DownAtMs = nowMs; s1Down = 1; }
        if (!s1new && s1Down)
        {
            if (nowMs - s1DownAtMs >= MENU_LONG_MS) ev->s1Long = 1;
            else                                    ev->s1Short = 1;
            s1Down = 0;
        }
        if (s2new && !s2Down) { s2DownAtMs = nowMs; s2Down = 1; }
        if (!s2new && s2Down)
        {
            if (nowMs - s2DownAtMs >= MENU_LONG_MS) ev->s2Long = 1;
            else                                    ev->s2Short = 1;
            s2Down = 0;
        }
    }
}

void Menu_Update(const MenuEvents *ev, uint32_t nowMs)
{
    switch (screen)
    {
    case M_LOGO:
        if (ev->s1Short || ev->s2Short || ev->s1Long || ev->s2Long)
        {
            screen = M_MAIN;
            Render_DemoIdle();
        }
        break;

    case M_MAIN:
        /* 滚轮：旋转切换曲目（8 段位置，两档一格）；S2 短按确认；S2 长按回上一页(LOGO)；
           S1 短按保留为下一首（防滚轮故障时的备用） */
        {
            uint8_t pos = Wheel_getPosition();
            if (wheelLast == 0xFFFF) wheelLast = pos;
            else if (pos != wheelLast)
            {
                int8_t d = (int8_t)pos - (int8_t)wheelLast;
                if (d > 4)  d -= 8;          /* 环绕：7→0 视为 +1 */
                if (d < -4) d += 8;          /* 0→7 视为 -1 */
                selected = (uint8_t)((selected + CHART_COUNT + d) % CHART_COUNT);
                wheelLast = pos;
            }
        }
        if (ev->s1Short) selected = (uint8_t)((selected + 1) % CHART_COUNT);
        if (ev->s2Short)
        {
            Play_Start(CHART_list[selected]);
            screen = M_PLAY;
        }
        if (ev->s2Long)                       /* 回退上一页 */
        {
            screen = M_LOGO;
            Render_DemoIdle();
        }
        break;

    case M_PLAY:
        if (Play_State() == PL_RESULT)
        {
            if (resultShownAt == 0) resultShownAt = nowMs;
            else if (nowMs - resultShownAt >= RESULT_WAIT_MS) screen = M_RESULT;
            break;
        }
        resultShownAt = 0;
        if (Play_State() == PL_READY)
        {
            if (ev->s2Short) Play_Go();        /* 只做跳过倒计时，不再走暂停分支 */
        }
        else if (ev->s2Short)
        {
            Play_PauseResume(nowMs);
            screen = (Play_State() == PL_PAUSED) ? M_PAUSE : M_PLAY;
        }
        if (Play_State() != PL_READY && ev->s1Long)
        {
            screen = M_MAIN;
            Play_ForceQuit();                  /* 干净地exit当前局 */
            Render_DemoIdle();
        }
        break;

    case M_PAUSE:
        if (ev->s2Short) Play_PauseResume(nowMs);          /* 继续 */
        if (ev->s1Short) { Play_Start(CHART_list[selected]); screen = M_PLAY; }  /* 重开 */
        if (ev->s1Long)  { screen = M_MAIN; Render_DemoIdle(); }
        break;

    case M_RESULT:
        if (ev->s2Short) { Play_Start(CHART_list[selected]); screen = M_PLAY; }
        if (ev->s1Short) { screen = M_MAIN; Render_DemoIdle(); }
        break;

    default:
        break;
    }
}
