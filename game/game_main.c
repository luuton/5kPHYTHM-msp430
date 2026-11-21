//============================================================================//
//  game_main.c - 5K 下落式节奏游戏：总装配（主循环） + P0 演示
//
//  入口：Game5K_Run()  —— 由 LAB1main.c 的主循环调用（替换 lab1()）
//
//  主循环按 project.md §4.3 六阶段：
//    1 触摸扫描窗口（占满 SCAN_BUDGET）
//    2 游戏逻辑（MISS 扫描并入 Play_Update）
//    3 渲染到影子
//    4 批量推屏（Render_* 内部完成）
//    5 菜单/按键轮询
//    6 帧对齐（每 16.7 ms）
//
//  击打时间戳 = 扫描中点（Touch_Scan 前后读 Now() 取中值）。
//============================================================================//
#include <stdint.h>
#include "msp430.h"
#include "HAL_Board.h"
#include "HAL_Buttons.h"
#include "HAL_Dogs102x6.h"
#include "game_main.h"
#include "game_time.h"
#include "game_touch.h"
#include "game_rtlcd.h"
#include "game_render.h"
#include "game_play.h"
#include "game_judge.h"
#include "game_menu.h"
#include "game_chart.h"
#include "game_telem.h"
#include "game_save.h"
#include "game_usb.h"

#define FW_TAG          "5k-r62"

/* ============================================================================
   隔离测试开关：1 = 开机只跑 USB，不初始化 CTS/按键/旋钮/TA0，
   也不进入游戏主循环。

   目的：LAB4 在同样的板子/线/口上能枚举，本工程不能。两者差异只剩
   "初始化了什么、主循环在做什么"。把游戏侧全部摘掉后：
     · 若能枚举  → 是 CTS/渲染/按键初始化或主循环干扰（几百 ms 的
                   D+ 上升沿窗口被抢占，或某外设改动了共享寄存器）
     · 若仍不能 → 是 USB 初始化本身（描述符 / msc_Init 缺失 / 端点配置）
   测完请改回 0。
   ============================================================================ */
#ifndef USB_STANDALONE_TEST
#define USB_STANDALONE_TEST   0
#endif

#define FRAME_MS        16u          /* 60 fps */
#define SCAN_BUDGET_MS  4u           /* 阶段 1 上限（E0.2 实测调整） */

static MenuEvents me;
static uint16_t  delta[5];

/* 遥测统计（每秒汇总一次） */
static uint16_t  tScanMin = 0xFFFF, tScanMax = 0, tScanAvg = 0;
static uint16_t  tScanN = 0;
static uint32_t  tScanSumUs = 0;
static uint16_t  tWorstFrame = 0;
static uint16_t  tBodyMax  = 0;   /* 每秒内：帧体（从帧开始到开始等待）最长 ms */
static uint16_t  tRenderMax = 0;  /* 每秒内：渲染阶段最长 us */
static uint16_t  tDrawMax = 0, tPresMax = 0;   /* 每秒内：绘制/推屏最长 us */
static uint16_t  tDeltaMin[5] = {0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF};
static uint16_t  tDeltaMax[5] = {0,0,0,0,0};
static uint32_t  tStatAtMs = 0;
static uint8_t   pressed[5];
static uint8_t   pressedPrev;
static uint8_t   edgesNow;
static const uint8_t laneLed[5] = { LED4, LED5, LED6, LED7, LED8 }; 

/* ---------- 小工具：u16 → 定宽 5 位十进制 ---------- */
static void u16toDec(uint16_t v, char *b)
{
    b[0] = (char)('0' + (v / 10000) % 10);
    b[1] = (char)('0' + (v / 1000) % 10);
    b[2] = (char)('0' + (v / 100) % 10);
    b[3] = (char)('0' + (v / 10) % 10);
    b[4] = (char)('0' + v % 10);
    b[5] = 0;
}

/* 一个扫描窗口：从 frameStart 起一直扫到 deadlineMs（帧内相对时刻）。
   时间戳取每次扫描的中点，与渲染帧率解耦。 */
static void scan_window(uint32_t frameStart, uint32_t deadlineMs)
{
    while (Time_NowMs() - frameStart < deadlineMs)
    {
        uint32_t t0 = Time_NowMs();
        uint8_t  hit;
        uint32_t mid;
        uint32_t scanUs;
        {
            uint32_t us0 = Time_NowUs();
            hit = Touch_Scan(delta, pressed);
            scanUs = Time_NowUs() - us0;              /* µs */
        }
        mid = t0 + (Time_NowMs() - t0) / 2u;
        /* 扫描耗时统计 */
        if ((uint16_t)scanUs < tScanMin) tScanMin = (uint16_t)scanUs;
        if ((uint16_t)scanUs > tScanMax) tScanMax = (uint16_t)scanUs;
        tScanSumUs += scanUs;
        tScanN++;

        /* 长条尾判需要"松开"这个边沿，单独取一份 */
        {
            uint8_t rel = (uint8_t)(pressedPrev & (uint8_t)~hit);
            edgesNow = (uint8_t)(hit & (uint8_t)~pressedPrev);
            pressedPrev = hit;

            /* 先查"到尾时是否还按着"，再处理松手与按下 */
            Play_HoldTick(mid, hit, delta);
            {
                uint8_t lane;
                for (lane = 0; lane < 5; lane++)
                    if (rel & (1u << lane))
                        Play_HoldRelease(lane, mid, delta);
            }
        }

        {   /* delta 区间统计（不管是否判为按下） */
            uint8_t ln;
            for (ln = 0; ln < 5; ln++)
            {
                if (delta[ln] < tDeltaMin[ln]) tDeltaMin[ln] = delta[ln];
                if (delta[ln] > tDeltaMax[ln]) tDeltaMax[ln] = delta[ln];
            }
        }

        {
            uint8_t lane;
            for (lane = 0; lane < 5; lane++)
                if (edgesNow & (1u << lane))
                {
                    Play_Tap(lane, mid, delta);
                }
        }
    }
}

/* ------------------------ 主循环（不返回） ------------------------ */
void Game5K_Run(void)
{
    uint32_t frameStart;

#if USB_STANDALONE_TEST
    /* ---- 隔离模式：只做 USB ---- */
    {
        uint16_t tick = 0;
        RTLCD_Init();          /* LCD 已由 LAB1main 初始化，这里只建影子缓冲 */
        Telem_Init();
        Usb_Init();
        while (1)
        {
            Usb_Poll();
            Render_Boot(Usb_StateStr(), tick++);
            __delay_cycles(25000UL * 16UL);   /* 约 16 ms（MCLK 24.97 MHz） */
        }
    }
#endif

    Telem_Init();                            /* 后端初始化（USB 模式下为空操作） */
    /* USB 不在开机时初始化。
       参照 LAB4 的做法：先让系统完全跑起来，由用户主动打开
       （主菜单 S1 长按 -> 设置界面 -> 选到 USB -> S2 加到 ON）。
       好处是 USB 启动时时钟/电源已稳定，且 D+ 上拉后的枚举窗口
       不会被开机初期的一堆初始化占用。 */

    Save_LoadSettings();                     /* 读回掉电保持的设置（在横幅之前，
                                                这样 V 行报的就是实际生效的参数） */
    /* 开机横幅：记录本固件的可调参数。
       USB 模式下此时可能还没枚举完，Telem 内部会挂起并在 ready 后补发
       （见 Telem_Flush 里的待发逻辑），不会丢。 */
    Telem_Version(FW_TAG, Play_SpeedPxPerS(), Render_NoteAlignPx,
                  JDG_WIN_PERFECT, JDG_WIN_GREAT, JDG_WIN_GOOD,
                  Touch_Thresholds);
    Touch_Init(8);
    RTLCD_Init();
    Menu_Init();
    Board_ledOff(LED_ALL);
    Time_Init();
    Buttons_interruptDisable(BUTTON_ALL);   /* 改轮询，禁用 HAL 的 P1/P2 ISR */
    buttonsPressed = 0;

    Render_DemoIdle();

    while (1)
    {
        frameStart = Time_NowMs();
        edgesNow = 0;

        /* -- USB 枚举等待：把 CPU 完全让给 USB --
           D+ 上拉后主机要连续做几十个 EP0 事务（几百 ms）。本工程每帧有
           4 ms 触摸扫描 + 整屏推屏（每次关中断 340 µs × 8），任何一次撞上
           都会让某个 EP0 事务超时 —— 实测表现为枚举随机卡在设备描述符或
           某个字符串描述符上。LAB4 能成功，正是因为它这段主循环什么都不做。
           这里复制该行为：不扫描、不渲染游戏，进度页只在秒数变化时才推屏
           （同样是为了避免推屏关中断）。 */
        if (Menu_UsbWaitTick())
        {
            static uint16_t lastTenth = 0xFFFFu;
            uint16_t tenth = Menu_UsbWaitTenths();

            Usb_Poll();
            Menu_ScanEvents(&me, frameStart);
            Menu_Update(&me, frameStart);          /* 处理 S1 长按取消 */

            if (tenth != lastTenth)
            {
                lastTenth = tenth;
                Render_UsbWait(Usb_StateStr(), tenth);
            }
            while (Time_NowMs() - frameStart < FRAME_MS) ;
            continue;
        }

        /* -- 阶段 1：触摸扫描窗口（时间戳 = 扫描中点） -- */
        scan_window(frameStart, SCAN_BUDGET_MS);

        /* -- 阶段 1.5：按住即亮 + 遥测出队（都在扫描窗口之外，不污染采样率） -- */
        {
            uint8_t lane, mask = 0;
            for (lane = 0; lane < 5; lane++)
                if (pressedPrev & (1u << lane))
                    mask |= laneLed[lane];
            Board_ledOff(LED_ALL);
            Board_ledOn(mask);
        }
        Telem_Flush();
        Usb_Poll();                              /* 非阻塞，几微秒 */

        /* -- 阶段 5：菜单键轮询 + 状态机 -- */
        Menu_ScanEvents(&me, frameStart);
        Menu_Update(&me, frameStart);
        gnow_last_ms = frameStart;

        /* -- 阶段 2/3/4：按菜单屏幕选择渲染内容 -- */
        {
            uint32_t r0 = Time_NowUs();
            uint8_t st = Menu_Screen();
            switch (st)
            {
            case 1:  /* M_MAIN */
                Render_MainMenu(Menu_SelectedChart());
                break;
            case 2:  /* M_PLAY */
                Play_Update(frameStart, edgesNow, 0, 0, 0);
                {
                    uint8_t ps = Play_State();
                    if (ps == PL_READY)
                        Render_Ready(Play_ReadyRemainMs());
                    else if (ps == PL_PLAYING)
                        Render_Frame(pressedPrev);
                    else if (ps == PL_PAUSED)
                        Render_Paused();
                    else if (ps == PL_RESUME)
                        Render_Resume(pressedPrev, Play_ResumeRemainMs());
                    else   /* PL_RESULT */
                        Render_Result();
                }
                break;
            case 3:  /* M_PAUSE（菜单暂停页） */
                Play_Update(frameStart, 0, 0, 0, 0);
                Render_Paused();
                break;
            case 4:  /* M_RESULT */
                Render_Result();
                break;
            case 5:  /* M_SETTINGS */
                Render_Settings(Menu_SettingsSel());
                break;
            default: /* M_LOGO：每帧刷新，USB 状态与心跳都可见 */
                Render_Boot(Usb_StateStr(), (uint16_t)(frameStart / 100u));
                break;
            }
            {
                uint32_t rUs = Time_NowUs() - r0;
                if ((uint16_t)rUs > tRenderMax) tRenderMax = (uint16_t)rUs;
            }
        }

        /* -- 阶段 5.5：把本帧剩余的空闲时间也用于扫描 --
           原来每帧只在前 4 ms 采样，剩下十几毫秒是输入盲区：落在盲区里的
           一次按压要等到下一帧才被记录（最多晚一整帧）。现在把渲染后的空闲
           时间也用来扫描，盲区从约 12 ms 缩到约 3 ms。 */
        {   /* 帧体耗时：到这里渲染已完成，剩余时间是纯空闲 */
            uint32_t bodyMs = Time_NowMs() - frameStart;
            if ((uint16_t)bodyMs > tBodyMax) tBodyMax = (uint16_t)bodyMs;
            if (Render_DrawUs    > tDrawMax) tDrawMax = Render_DrawUs;
            if (Render_PresentUs > tPresMax) tPresMax = Render_PresentUs;
        }
        scan_window(frameStart, FRAME_MS - 3u);

        /* -- 阶段 6：帧对齐 -- */
        while (Time_NowMs() - frameStart < FRAME_MS) ;
        {
            uint32_t frameMs = Time_NowMs() - frameStart;
            if ((uint16_t)frameMs > tWorstFrame) tWorstFrame = (uint16_t)frameMs;
            if (frameMs > 500u) Telem_Stall(frameMs, Time_NowMs());

            /* 每秒输出一行帧/扫描统计 */
            if (frameStart - tStatAtMs >= 1000u)
            {
                tScanAvg = tScanN ? (uint16_t)(tScanSumUs / tScanN) : 0;
                Telem_FrameStats(tWorstFrame, tBodyMax, tRenderMax,
                                 tScanN ? tScanMin : 0, tScanAvg,
                                 tScanN ? tScanMax : 0,
                                 tDrawMax, tPresMax);
                /* 独立时钟对照：游戏计时秒数 vs RTC 秒数（ACLK 32.768k 驱动） */
                if (RTCCTL01 & RTCRDY) Telem_Clock(frameStart / 1000u, RTCSEC);
                Telem_DeltaRange(tDeltaMin, tDeltaMax);
                /* 演奏中额外打一行状态：用来追"铺面消失"这类问题 */
                if (Menu_Screen() == 2u)   /* M_PLAY */
                    Telem_Play((uint8_t)Play_State(), Play_SongTimeMs(),
                               Judge_Head(), Judge_NoteCount());
                tWorstFrame = 0; tBodyMax = 0; tRenderMax = 0; tDrawMax = 0; tPresMax = 0;
                tScanMin = 0xFFFF; tScanMax = 0; tScanSumUs = 0; tScanN = 0;
                {
                    uint8_t ln;
                    for (ln = 0; ln < 5; ln++)
                    { tDeltaMin[ln] = 0xFFFF; tDeltaMax[ln] = 0; }
                }
                tStatAtMs = frameStart;
            }
        }
    }
}

/* ====================== P0 演示（lab 菜单可选） ====================== */

/* E0.1：8x12px 方块每帧右移 1 px；S1 切换 60/30/15 fps；S2 退出 */
void Demo_LcdMotion(void)
{
    static const uint8_t frame_ms_tab[3] = { 16, 33, 66 };
    uint8_t fps_sel = 0;
    uint8_t x = 0, y = 20;
    uint32_t fs;

    Time_Init();
    Buttons_init(BUTTON_ALL);
    Buttons_interruptEnable(BUTTON_ALL);
    buttonsPressed = 0;

    while (1)
    {
        fs = Time_NowMs();
        RTLCD_Clear();
        RTLCD_FillRect(x, y, (uint8_t)(x + 11), (uint8_t)(y + 11), 1);
        RTLCD_Present();

        if (++x > 89) { x = 0; y = (y == 20) ? 40 : 20; }

        if (buttonsPressed & BUTTON_S1)
        {
            fps_sel = (uint8_t)((fps_sel + 1) % 3);
            buttonsPressed = 0;
        }
        if (buttonsPressed & BUTTON_S2) { buttonsPressed = 0; return; }

        while (Time_NowMs() - fs < frame_ms_tab[fps_sel]) ;
    }
}

/* E0.2：CTS 全扫耗时统计（500 次，min/avg/max µs） */
void Demo_ScanTiming(void)
{
    uint32_t t0, t1, sum = 0;
    uint16_t n, tmin = 0xFFFF, tmax = 0;
    char b1[7], b2[7];
    uint16_t avg;

    Touch_Init(16);
    Time_Init();
    buttonsPressed = 0;

    for (n = 0; n < 500; n++)
    {
        t0 = Time_NowUs();
        Touch_RawScan(delta);
        t1 = Time_NowUs() - t0;                  /* µs */
        sum += t1;
        if (t1 < tmin) tmin = (uint16_t)t1;
        if (t1 > tmax) tmax = (uint16_t)t1;
    }

    avg = (uint16_t)(sum / 500u);
    RTLCD_Clear();
    RTLCD_String(0, 0, "SCAN TIMING (us)", 0);
    RTLCD_String(0, 16, "MIN", 0); u16toDec(tmin, b1); RTLCD_String(48, 16, b1, 0);
    RTLCD_String(0, 28, "AVG", 0); u16toDec(avg,  b2); RTLCD_String(48, 28, b2, 0);
    RTLCD_String(0, 40, "MAX", 0); u16toDec(tmax, b1); RTLCD_String(48, 40, b1, 0);
    RTLCD_Present();

    buttonsPressed = 0;
    while (!buttonsPressed) ;
    buttonsPressed = 0;
}

/* P3：触摸标定画面（柱状 + 阈值线 + 顶部两位数值） */
/* P3：触摸标定。屏显柱状图，同时把 当前 delta / 峰值 / 阈值 走串口发出。
   峰值保持是标定的关键：单次按压的 delta 只持续几帧，靠眼睛抓不住，
   峰值能把"按下去最大到多少"记录下来。
   操作：S1 清峰值；S2 退出。 */
void Demo_Calib(void)
{
    uint16_t peak[5] = { 0, 0, 0, 0, 0 };
    uint8_t  i;
    uint16_t printCnt = 0;

    Touch_Init(8);
    Time_Init();
    buttonsPressed = 0;

    while (!(buttonsPressed & BUTTON_S2))
    {
        uint32_t fs = Time_NowMs();

        Touch_RawScan(delta);
        for (i = 0; i < 5; i++)
            if (delta[i] > peak[i]) peak[i] = delta[i];

        Render_Calibration(delta, Touch_Thresholds);

        /* 串口每 250 ms 打一行，便于复制取值 */
        if (++printCnt >= 5)
        {
            printCnt = 0;
            Telem_Calib(delta, peak, Touch_Thresholds);
        }

        if (buttonsPressed & BUTTON_S1)
        {
            for (i = 0; i < 5; i++) peak[i] = 0;      /* 清峰值，测下一路 */
            buttonsPressed = 0;
        }
        while (Time_NowMs() - fs < 50) ;
    }
    buttonsPressed = 0;
}
