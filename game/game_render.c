//============================================================================//
//  game_render.c - 渲染实现
//
//  帧流程（主循环阶段 3/4 调用）：
//    Render_Frame(pressedBits)
//      1. RTLCD_Clear()
//      2. 画状态栏：分数 / 连击 / 判定文字（6x8）
//      3. 画谱面音符：对 head..lookahead 的每个未判/已判音符按
//         y = JUDGE_Y - (t_ms - songNow)*speed/1000 画 16x4 实心块
//      4. 画判定线：y=55 实线；按住的轨把判定线区域反色填
//      5. RTLCD_Present() 批量推屏
//
//  成本：每次全量 816 B 推屏 ≈ 2.7 ms（打表，project.md §5.3）
//============================================================================//
#include <stdint.h>
#include "msp430.h"          /* 诊断页要直接读 UCSCTL / USBPLL / USBCNF 等寄存器 */
#include "HAL_Dogs102x6.h"
#include "game_rtlcd.h"
#include "game_render.h"
#include "game_judge.h"
#include "game_play.h"
#include "game_time.h"
#include "game_chart.h"
#include "game_usb.h"   /* Render_Settings 显示 USB ON/OFF */
#include "game_touch.h"  /* 设置页显示 Touch_SensPct */
#include "game_save.h"   /* 结算页/选曲页显示最高分 */

int16_t Render_NoteAlignPx = 3;   /* 见 game_render.h 说明；设置界面可改 */


static char numbuf[6];

/* 右对齐定宽无符号十进制，结果写入 numbuf */
static void fmt_u16(uint16_t v, uint8_t digits)
{
    int8_t i;
    for (i = (int8_t)digits - 1; i >= 0; i--) { numbuf[i] = (char)('0' + v % 10); v /= 10; }
    numbuf[digits] = 0;
}

/* 右对齐定宽带符号十进制（空格补齐，b 至少 digits+2 字节） */
static void fmt_i16(char *b, int16_t v, uint8_t digits)
{
    uint8_t i;
    uint16_t u = (uint16_t)(v < 0 ? -v : v);
    b[0] = (v < 0) ? '-' : '+';
    b[digits + 1] = 0;
    for (i = 0; i < digits; i++)
    {
        b[digits - i] = (u ? (char)('0' + u % 10) : ((i == 0) ? '0' : ' '));
        u /= 10;
    }
}

/* 把 16 进制值花式居中打印等（结算页用） */


/* 状态栏布局（6px/字符）：
     分数   5 位 @ x=0   （0..29）
     Combo  标签 @ x=30  （30..59）  ← 原为 24，会与分数末位重叠，右移一格
     连击数 4 位 @ x=62  （62..85）  ← 原为 5 位 @56，改为 4 位并右移一格 */
static void draw_status(void)
{
    fmt_u16((uint16_t)Judge_Score(), 5);
    RTLCD_String(0, 0, numbuf, 0);
    RTLCD_String(30, 0, "Combo", 0);
    fmt_u16(Judge_Combo(), 4);
    RTLCD_String(62, 0, numbuf, 0);
}

/* 长条宽度（居中在 16px 音符里） */
#define REN_HOLD_W   6

/* 画一段长条身体：竖直细条，夹在状态栏下沿与判定线之间。
   ytop/ybot 是屏幕坐标（y 向下增大）。 */
static void draw_hold_bar(int32_t x, int32_t ytop, int32_t ybot)
{
    int32_t x1 = x + (REN_NOTE_W - REN_HOLD_W) / 2;
    if (ybot < REN_PLAY_TOP_Y) return;
    if (ytop > REN_JUDGE_Y) return;
    if (ytop < REN_PLAY_TOP_Y) ytop = REN_PLAY_TOP_Y;
    if (ybot > REN_JUDGE_Y) ybot = REN_JUDGE_Y;
    if (ybot < ytop) return;
    RTLCD_FillRect((uint8_t)x1, (uint8_t)ytop,
                   (uint8_t)(x1 + REN_HOLD_W - 1), (uint8_t)ybot, 1);
}

/* 已命中的长条：头部已经判过、从 head 之后不再遍历到，
   所以单独按轨查询"尾巴结束时刻"，把还没走完的部分画出来。
   尾巴过线后自动不画（判定层不管尾端，玩家松手也不受罚）。 */
static void draw_hold_tails(void)
{
    uint32_t t = Play_SongTimeMs();
    int32_t  speed = (int32_t)Play_SpeedPxPerS();
    uint8_t  lane;
    for (lane = 0; lane < 5u; lane++)
    {
        uint32_t tail = Judge_HoldTailMs(lane);
        int32_t  ty;
        if (!Judge_HoldActive(lane) || t >= tail) continue;
        ty = REN_JUDGE_Y - (int32_t)(((int32_t)tail - (int32_t)t) * speed / 1000);
        draw_hold_bar(REN_LANE_X(lane) + 2, ty, REN_JUDGE_Y);
    }
}

static void draw_notes(void)
{
    const Note *nt = Judge_Notes();
    uint16_t n = Judge_NoteCount();
    uint16_t head = Judge_Head();
    uint32_t t = Play_SongTimeMs();
    uint16_t speed = Play_SpeedPxPerS();
    uint16_t i;

    /* 视窗：只遍历 head 起的音符，遇到"到达时间 > 判定线提前量"即停。
       提前量 = (REN_JUDGE_Y + REN_NOTE_H) / speed * 1000 ms。 */
    for (i = head; i < n && i < head + 24u; i++)
    {
        int32_t dtms = (int32_t)nt[i].t_ms - (int32_t)t;
        int32_t dy;
        int32_t y;
        int32_t x;
        if (dtms * (int32_t)speed / 1000 > REN_JUDGE_Y + 4)
            break;                        /* 还没进入屏幕（后面的更晚） */
        dy = (dtms * (int32_t)speed) / 1000;
        /* dt=0（判定时刻）时音符相对判定线的位置，由 REN_NOTE_ALIGN_PX 标定 */
        y  = REN_JUDGE_Y - dy - Render_NoteAlignPx;
        x  = REN_LANE_X(nt[i].lane) + 2;
        if (y > (int32_t)(RTLCD_H - REN_NOTE_H)) y = (int32_t)(RTLCD_H - REN_NOTE_H);
        if (nt[i].judged) continue;       /* 已判的不画（MISS/命中都消失） */
        if (nt[i].hold_ms)                /* 长条：先画身体，再画头 */
        {
            int32_t ty = y - (int32_t)((uint32_t)nt[i].hold_ms * (uint32_t)speed / 1000u);
            draw_hold_bar(x, ty, y + REN_NOTE_H - 1);
        }
        {
            /* 顶部 8 px 是状态栏：音符从状态栏底下冒出。
               音符底边 < 状态栏底 → 整块还在状态栏内，不画（否则 FillRect 的
               swap 会把反向矩形画到状态栏上）。 */
            int32_t y2 = y + REN_NOTE_H - 1;
            int32_t y1;
            if (y2 < REN_PLAY_TOP_Y) continue;
            y1 = (y < REN_PLAY_TOP_Y) ? REN_PLAY_TOP_Y : y;
            RTLCD_FillRect((uint8_t)x, (uint8_t)y1,
                           (uint8_t)(x + REN_NOTE_W - 1),
                           (uint8_t)y2, 1);
        }
    }
}

/* 实时判定显示：最近一次判定后 400 ms 内，在判定线正下方居中弹出
   PERFECT / GREAT / GOOD / MISS。居中按 6px/字符 计算。 */
static void draw_judgment(void)
{
    static const char *const txt[6] = { "", "MISS", "GOOD", "GREAT", "PERFECT", "" };
    uint8_t     r = Judge_LastResult();
    const char *s;
    uint8_t     n, x;

    /* 底部 8px 是判定专用区：先清干净。
       否则判定文字与音符同为"置位像素"，叠加后互相淹没、读不出来；
       而这一条本就在判定线下方，只影响已越过判定线、即将 MISS 的音符尾部。 */
    RTLCD_FillRect(0, REN_JUDGE_TXT_Y, RTLCD_W - 1, RTLCD_H - 1, 0);

    if (r == JR_NONE || r == JR_WRONG || r > JR_PERFECT) return;
    if (Play_SongTimeMs() - Judge_LastResultMs() > REN_JUDGE_TXT_MS) return;

    s = txt[r];
    for (n = 0; s[n]; n++) ;
    x = (uint8_t)((RTLCD_W - n * 6u) / 2u);
    RTLCD_String(x, REN_JUDGE_TXT_Y, s, 0);
}

static void draw_judgeline(uint8_t pressed)
{
    uint8_t lane;
    RTLCD_HLine(0, 101, REN_JUDGE_Y, 1);
    for (lane = 0; lane < 5; lane++)
    {
        if (pressed & (1u << lane))
        {
            /* 按住高亮：判定线上方 4 px 反色填 lane 区域 */
            RTLCD_FillRect(REN_LANE_X(lane), REN_JUDGE_Y - 5,
                           REN_LANE_X(lane) + REN_LANE_W - 1, REN_JUDGE_Y - 1, 1);
        }
    }
}

/* 只画不推屏：供 Render_Frame 与 Render_Resume 共用，
   避免倒计时画面推两次屏（每次 2.7 ms）。 */
static void render_scene(uint8_t lanePressedBits)
{
    RTLCD_Clear();
    draw_status();
    draw_notes();
    draw_hold_tails();
    draw_judgeline(lanePressedBits);
    draw_judgment();          /* 最后画，压在音符之上 */
}

/* 渲染耗时拆成两段，供遥测定位：绘制（纯 RAM 影子缓冲）与推屏（SPI 到 LCD）。 */
volatile uint16_t Render_DrawUs    = 0;
volatile uint16_t Render_PresentUs = 0;

void Render_Frame(uint8_t lanePressedBits)
{
    uint32_t t0 = Time_NowUs();
    render_scene(lanePressedBits);
    Render_DrawUs = (uint16_t)(Time_NowUs() - t0);
    t0 = Time_NowUs();
    RTLCD_Present();
    Render_PresentUs = (uint16_t)(Time_NowUs() - t0);
}

/* 暂停恢复倒计时：谱面冻结在暂停位置（songNow 未推进），
   中间压一个大号数字。数字底下先清一块，避免与下落音符同色糊在一起。 */
void Render_Resume(uint8_t lanePressedBits, uint32_t remainMs)
{
    char    c[2];
    uint8_t d;

    render_scene(lanePressedBits);

    d = (uint8_t)((remainMs + 999u) / 1000u);    /* 5,4,3,2,1 */
    if (d > 5u) d = 5u;
    c[0] = (char)('0' + d);
    c[1] = 0;

    /* 2x 字模每字符 12x16：(102-12)/2 = 45 */
    RTLCD_FillRect(42, 21, 59, 42, 0);
    RTLCD_String2x(45, 24, c, 0);
    RTLCD_Present();
}

void Render_Result(void)
{
    char buf[8];
    uint32_t sc   = Judge_Score();
    uint32_t best = Save_BestScore(Play_ChartIndex());
    uint8_t  i;

    RTLCD_Clear();
    /* 标题三选一：破纪录 > 全连 > 普通结算（都是 6 或 10 字符，居中） */
    if (Save_NewRecord())
        RTLCD_String(21, 2, "NEW RECORD", 0);
    else if (Judge_FullCombo())
        RTLCD_String(21, 2, "FULL COMBO", 0);
    else
        RTLCD_String(27, 2, "RESULT", 0);

    for (i = 0; i < 5; i++) { buf[4-i] = (char)('0' + (uint8_t)(sc % 10)); sc /= 10; }
    buf[5] = 0;
    RTLCD_String2x(9, 12, buf, 0);

    /* 逐判定行数 */
    {
        char row[12];
        const char *names[4] = { "PF", "GR", "GD", "MS" };
        for (i = 0; i < 4; i++)
        {
            uint16_t v = Judge_CountOf((uint8_t)(JR_PERFECT - i));
            row[0] = names[i][0]; row[1] = names[i][1]; row[2] = ':';
            row[3] = (char)('0' + (v / 100) % 10);
            row[4] = (char)('0' + (v / 10) % 10);
            row[5] = (char)('0' + v % 10);
            row[6] = 0;
            RTLCD_String(0, (uint8_t)(32 + i * 8), row, 0);
        }
        {   /* 右列：MAX 连击 + 该曲历史最高分 */
            char mc[10] = "MAX:000";
            char bc[12];
            uint16_t v = Judge_MaxCombo();
            uint32_t bs = best;

            mc[4] = (char)('0' + (v / 100) % 10);
            mc[5] = (char)('0' + (v / 10) % 10);
            mc[6] = (char)('0' + v % 10);
            RTLCD_String(48, 48, mc, 0);

            bc[0] = 'B'; bc[1] = 'S'; bc[2] = 'T'; bc[3] = ':';
            for (i = 0; i < 5; i++) { bc[8 - i] = (char)('0' + (uint8_t)(bs % 10)); bs /= 10; }
            bc[9] = 0;
            RTLCD_String(48, 56, bc, 0);
        }
    }
    RTLCD_Present();
}

void Render_Boot(const char *usbTxt, uint16_t tick)
{
    char b[8];
    uint8_t i;

    RTLCD_Clear();
    RTLCD_String(15, 6, "5K RHYTHM", 0);
    RTLCD_String(6, 20, "Press any key", 0);
    if (usbTxt) RTLCD_String(6, 34, usbTxt, 0);

    /* 心跳：每 100 ms 变一次。数字不动说明主循环没在推进。 */
    b[0] = 't'; b[1] = '=';
    for (i = 0; i < 4; i++) b[2 + i] = (char)('0' + (tick / (uint16_t)(i == 0 ? 1000 : (i == 1 ? 100 : (i == 2 ? 10 : 1)))) % 10);
    b[6] = 0;
    RTLCD_String(6, 48, b, 0);

    RTLCD_Present();
}

/* 兼容旧调用点（菜单回主界面等处） */
void Render_DemoIdle(void)
{
    Render_Boot(0, 0);
}

void Render_Calibration(const uint16_t *delta, const uint16_t *thr)
{
    uint8_t lane;
    char line[13];
    RTLCD_Clear();
    RTLCD_String(0, 0, "CAL (live)", 0);
    for (lane = 0; lane < 5; lane++)
    {
        uint16_t d = delta[lane];
        uint16_t h = (uint16_t)(d / 8);        /* 400 → 50 px 高 */
        uint8_t  c = 0;
        if (h > 50) h = 50;
        /* 每轨 16 px 宽柱，x 基 = 2 + lane*20 */
        while (c < h && c < 48)
        {
            RTLCD_FillRect((uint8_t)(lane * 20 + 2), (uint8_t)(62 - c),
                           (uint8_t)(lane * 20 + 17), 62, 1);
            c++;
        }
        /* 阈值水平线（柱内） */
        RTLCD_HLine((uint8_t)(lane * 20 + 2), (uint8_t)(lane * 20 + 17),
                    (uint8_t)(62 - thr[lane] / 8), 0);
        /* 数值：d 值 4 位要 24 宽 → 5 轨排不下；只画 2 位大格子？缩为 d/10 */
        {
            uint8_t v2 = (uint8_t)(d / 10);
            line[0] = (char)('0' + v2 / 10);
            line[1] = (char)('0' + v2 % 10);
            line[2] = 0;
            RTLCD_String((uint8_t)(lane * 20 + 2), 0, line, (uint8_t)(h > (thr[lane] / 8) ? 1 : 0));
        }
    }
    RTLCD_Present();
}

void Render_Init(void)
{
    RTLCD_Init();
}


void Render_MainMenu(uint8_t selected)
{
    uint8_t i;
    /* 曲名不放 BPM：原来 "2. Normal 136" 长达 13 字符(78px)，
       再在 x=66 画最高分会直接压到名字上、溢出到屏幕外。 */
    static const char *const names[3] = { "Easy", "Normal", "Hard" };
    char line[16];
    RTLCD_Clear();
    RTLCD_String(0, 0, "SELECT SONG", 0);
    for (i = 0; i < 3; i++)
    {
        line[0] = (char)('0' + i + 1);
        line[1] = '.';
        line[2] = 0;
        {
            int8_t k;
            for (k = 0; names[i][k] && k < 8; k++) line[2 + k] = names[i][k];
            line[2 + k] = 0;
        }
        RTLCD_String(0, (uint8_t)(16 + i * 12), line, (uint8_t)(i == selected));

        {   /* 历史最高分：没打过显示 ----- */
            char sb[8];
            uint32_t bs = Save_BestScore(i);
            uint8_t k;
            if (bs == 0)
            {
                sb[0] = '-'; sb[1] = '-'; sb[2] = '-'; sb[3] = '-'; sb[4] = '-'; sb[5] = 0;
            }
            else
            {
                for (k = 0; k < 5; k++) { sb[4 - k] = (char)('0' + (uint8_t)(bs % 10)); bs /= 10; }
                sb[5] = 0;
            }
            RTLCD_String(72, (uint8_t)(16 + i * 12), sb, (uint8_t)(i == selected));
        }
    }
    RTLCD_String(0, 48, "Whl:song  S2:go", 0);
    RTLCD_String(0, 56, "S1hold:SETTINGS", 0);
    RTLCD_Present();
}

void Render_Ready(uint32_t remainMs)
{
    char c[2];
    RTLCD_Clear();
    c[0] = (remainMs > 800u) ? '3' : (remainMs > 400u) ? '2' : '1';
    c[1] = 0;
    RTLCD_String2x(45, 24, c, 0);
    RTLCD_Present();
}

/* 暂停页。居中按 6px/字符：(102 - 字符数*6)/2
   PAUSED     6 字符 -> x=33
   S2:resume  9 字符 -> x=24
   S1:restart 10 字符 -> x=21
   S1hold:quit 11 字符 -> x=18
   原实现把 PAUSED 放在 x=21（未居中），且漏了"长按 S1 弃局"的提示。 */
void Render_Paused(void)
{
    RTLCD_Clear();
    RTLCD_String(33, 14, "PAUSED", 0);
    RTLCD_String(24, 32, "S2:resume", 0);
    RTLCD_String(21, 42, "S1:restart", 0);
    RTLCD_String(18, 52, "S1hold:quit", 0);
    RTLCD_Present();
}

/* ============================ 设置界面 ============================
   布局（6x8 字体，行距 9~10 px）：
     y=0   SETTINGS
     y=10  >OFF  <判定偏移 ms>      可调 -100..+100 步 5
     y=19   ALN  <视觉对齐 px>      可调 -2..+6   步 1
     y=28   SPD  <下落速度 px/s>    可调 40..120  步 10
     y=37   WIN  <PERFECT 窗口 ms>  可调 15..60   步 5（GREAT/GOOD 按 1:2:10/3 联动）
     y=46  dt avg <最近一局 dt 均值> 只读，用来标定 OFF
     y=54  按键提示
   标定方法：跑一局 → 看 dt avg → 把 OFF 调到 -dt avg（步 5）→ 再跑一局确认接近 0。 */
void Render_Settings(uint8_t sel)
{
    static const char *const names[7] = { "OFF", "ALN", "SPD", "WIN", "USB", "SENS", "CLR" };
    char b[10];
    uint8_t i;

    RTLCD_Clear();
    RTLCD_String(0, 0, "SETTINGS", 0);
    {   /* dt 均值挪到标题行右侧，给第 7 项腾出空间 */
        fmt_i16(b, (int16_t)Judge_DtMeanMs(), 3);
        RTLCD_String(60, 0, "dt", 0);
        RTLCD_String(72, 0, b, 0);
    }

    for (i = 0; i < 7; i++)
    {
        uint8_t y = (uint8_t)(8 + i * 8);        /* 8..56，7 行 */
        b[0] = (i == sel) ? '>' : ' ';
        b[1] = 0;
        RTLCD_String(0, y, b, 0);
        RTLCD_String(6, y, names[i], 0);

        if (i == 4)                              /* USB：显示 ON/OFF 而非数字 */
        {
            RTLCD_String(42, y, Usb_IsInited() ? "ON" : "OFF", 0);
        }
        else if (i == 5)                         /* 灵敏度：百分比 */
        {
            fmt_i16(b, (int16_t)Touch_SensPct, 3);
            RTLCD_String(42, y, b, 0);
            RTLCD_String(66, y, "%", 0);
        }
        else if (i == 6)                         /* 清空记录：按 S2 执行 */
        {
            RTLCD_String(38, y, "S2:wipe", 0);
        }
        else
        {
            switch (i)
            {
            case 0:  fmt_i16(b, (int16_t)JDG_OFFSET_MS, 3);      break;
            case 1:  fmt_i16(b, Render_NoteAlignPx, 2);          break;
            case 2:  fmt_i16(b, (int16_t)Play_SpeedPxPerS(), 3); break;
            default: fmt_i16(b, (int16_t)JDG_WIN_PERFECT, 2);    break;
            }
            RTLCD_String(42, y, b, 0);
        }
    }

    RTLCD_Present();
}


/* USB 枚举等待页 */
void Render_UsbWait(const char *usbTxt, uint16_t tenthSec)
{
    char b[8];
    RTLCD_Clear();
    RTLCD_String(18, 8, "USB ENUM", 0);
    if (usbTxt) RTLCD_String(6, 24, usbTxt, 0);
    b[0] = (char)('0' + (tenthSec / 10) % 10);
    b[1] = '.';
    b[2] = (char)('0' + tenthSec % 10);
    b[3] = 's'; b[4] = 0;
    RTLCD_String(36, 40, b, 0);
    RTLCD_String(6, 54, "S1hold:cancel", 0);
    RTLCD_Present();
}
