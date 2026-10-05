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
#include "game_telem.h"
#include "game_touch.h"
#include "game_save.h"

enum { M_LOGO = 0, M_MAIN = 1, M_PLAY = 2, M_PAUSE = 3, M_RESULT = 4, M_SETTINGS = 5 };

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
static uint8_t  setSel = 0;              /* 设置界面光标项 0..3 */

uint8_t Menu_Screen(void)         { return screen; }
uint8_t Menu_SelectedChart(void)  { return selected; }
uint8_t Menu_SettingsSel(void)    { return setSel; }



/* 非阻塞读旋钮位置。
   为什么不用 HAL 的 Wheel_getPosition()：它内部执行
   __bis_SR_register(LPM0_bits + GIE) 等 ADC 中断，一旦该中断丢失就永久睡死
   （本项目实测出现过一次 24823 ms 的"单帧"，正发生在调用旋钮读取的菜单界面；
   且 lab1.c 的 RTC_ISR 只在 RTCExitSec==1 时才清 LPM 位，故 RTC 中断也唤不醒它）。
   这里改为有界轮询 + 超时返回无效值，绝不挂死。
   抖动门限沿用 HAL 的 ±10 counts。 */
#define WHEEL_POLL_GUARD  20000u
#define WHEEL_HYST        10u

static uint8_t wheel_pos(void)
{
    static uint16_t rawOld = 0xFFFFu;
    uint16_t g = 0;
    uint16_t v;

    ADC12IFG &= ~BIT0;
    ADC12CTL0 |= ADC12SC;                       /* 启动一次转换 */
    while (!(ADC12IFG & BIT0) && g < WHEEL_POLL_GUARD) g++;
    if (!(ADC12IFG & BIT0)) return 0xFFu;       /* 超时：本次无效 */
    v = ADC12MEM0;

    if (rawOld != 0xFFFFu)
    {
        uint16_t d = (v > rawOld) ? (uint16_t)(v - rawOld) : (uint16_t)(rawOld - v);
        if (d <= WHEEL_HYST) v = rawOld;        /* 抖动门限内沿用旧值 */
    }
    rawOld = v;

    return (v > 0x0806u) ? (uint8_t)(7u - (v - 0x0806u) / 260u)
                         : (uint8_t)(v / 260u);
}

/* 滚轮增量：返回自上次调用以来的档位变化（已做 8 档环绕修正）。
   M_MAIN 与 M_SETTINGS 共用；切换界面时把 wheelLast 置 0xFFFF 重新同步。 */
static int8_t wheel_delta(void)
{
    uint8_t pos = wheel_pos();
    int8_t d;
    if (pos == 0xFFu) return 0;                 /* 读取超时：本次不产生增量 */
    if (wheelLast == 0xFFFF) { wheelLast = pos; return 0; }
    if (pos == wheelLast) return 0;
    d = (int8_t)pos - (int8_t)wheelLast;
    if (d >  4) d -= 8;
    if (d < -4) d += 8;
    wheelLast = pos;
    return d;
}

/* 数值限幅 */
static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

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
        /* 滚轮：切换曲目；S2 短按确认；S2 长按回 LOGO；S1 长按进设置界面；
           S1 短按保留为下一首（防滚轮故障时的备用） */
        {
            int8_t d = wheel_delta();
            if (d) selected = (uint8_t)((selected + CHART_COUNT + d) % CHART_COUNT);
        }
        if (ev->s1Short) selected = (uint8_t)((selected + 1) % CHART_COUNT);
        if (ev->s1Long)                       /* 进设置界面 */
        {
            screen = M_SETTINGS;
            setSel = 0;
            wheelLast = 0xFFFF;               /* 重新同步滚轮基准 */
        }
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

    case M_SETTINGS:
        /* 滚轮：选项目；S1 短按减；S2 短按加；S2 长按保存并返回。
           值域与步进见 Render_Settings 注释。 */
        {
            int8_t d = wheel_delta();
            if (d) setSel = (uint8_t)clamp_i32((int32_t)setSel + d, 0, 3);
        }
        {
            int8_t  dir = 0;
            if (ev->s1Short) dir = -1;
            if (ev->s2Short) dir = +1;
            if (dir)
            {
                switch (setSel)
                {
                case 0:  /* 判定偏移 ms，步 5 */
                    JDG_OFFSET_MS = clamp_i32(JDG_OFFSET_MS + dir * 5, -100, 100);
                    break;
                case 1:  /* 视觉对齐 px，步 1 */
                    Render_NoteAlignPx = (int16_t)clamp_i32(Render_NoteAlignPx + dir, -2, 6);
                    break;
                case 2:  /* 下落速度 px/s，步 10 */
                    Play_SetSpeedPxPerS((uint16_t)clamp_i32(
                        (int32_t)Play_SpeedPxPerS() + dir * 10, 40, 120));
                    break;
                default: /* PERFECT 窗口 ms，步 5；GREAT/GOOD 按 1:2:10/3 联动 */
                    JDG_WIN_PERFECT = clamp_i32(JDG_WIN_PERFECT + dir * 5, 15, 60);
                    JDG_WIN_GREAT   = JDG_WIN_PERFECT * 2;
                    JDG_WIN_GOOD    = JDG_WIN_PERFECT * 10 / 3;
                    break;
                }
            }
        }
        if (ev->s2Long)                       /* 落盘 + 返回；顺便把调好的参数打进串口 */
        {
            Save_StoreSettings();             /* 写 INFO D，掉电保持 */
            Telem_Version("tuned", Play_SpeedPxPerS(), Render_NoteAlignPx,
                          JDG_WIN_PERFECT, JDG_WIN_GREAT, JDG_WIN_GOOD,
                          Touch_Thresholds);
            screen = M_MAIN;
            wheelLast = 0xFFFF;               /* 回主菜单重新同步 */
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
