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
#include "HAL_Dogs102x6.h"
#include "game_rtlcd.h"
#include "game_render.h"
#include "game_judge.h"
#include "game_play.h"
#include "game_chart.h"

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

void Render_Frame(uint8_t lanePressedBits)
{
    RTLCD_Clear();
    draw_status();
    draw_notes();
    draw_judgeline(lanePressedBits);
    RTLCD_Present();
}

void Render_Result(void)
{
    char buf[8];
    uint32_t sc = Judge_Score();
    uint8_t  i;

    RTLCD_Clear();
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
            RTLCD_String(0, (uint8_t)(34 + i * 8), row, 0);
        }
        if (Judge_FullCombo()) RTLCD_String(48, 58, "FULL COMBO", 0);
        else
        {
            char mc[10] = "MAX:000";
            uint16_t v = Judge_MaxCombo();
            mc[4] = (char)('0' + (v / 100) % 10);
            mc[5] = (char)('0' + (v / 10) % 10);
            mc[6] = (char)('0' + v % 10);
            RTLCD_String(48, 58, mc, 0);
        }
    }
    RTLCD_Present();
}

void Render_DemoIdle(void)
{
    RTLCD_Clear();
    RTLCD_String(15, 8, "5K RHYTHM", 0);
    RTLCD_String(6, 22, "Press any key", 0);
    RTLCD_Present();
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
    static const char *const names[3] = { "Easy 120", "Normal 136", "Hard 150" };
    char line[16];
    RTLCD_Clear();
    RTLCD_String(0, 0, "SELECT SONG", 0);
    for (i = 0; i < 3; i++)
    {
        line[0] = (char)('0' + i + 1);
        line[1] = '.';
        line[2] = ' ';
        line[3] = 0;
        {
            int8_t k;
            for (k = 0; names[i][k] && k < 12; k++) line[3 + k] = names[i][k];
            line[3 + k] = 0;
        }
        RTLCD_String(0, (uint8_t)(16 + i * 12), line, (uint8_t)(i == selected));
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

void Render_Paused(void)
{
    RTLCD_Clear();
    RTLCD_String(21, 24, "PAUSED", 0);
    RTLCD_String(6, 40, "S2:resume", 0);
    RTLCD_String(6, 52, "S1:restart", 0);
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
    static const char *const names[4] = { "OFF", "ALN", "SPD", "WIN" };
    char b[10];
    uint8_t i;

    RTLCD_Clear();
    RTLCD_String(0, 0, "SETTINGS", 0);

    for (i = 0; i < 4; i++)
    {
        uint8_t y = (uint8_t)(10 + i * 9);
        b[0] = (i == sel) ? '>' : ' ';
        b[1] = 0;
        RTLCD_String(0, y, b, 0);
        RTLCD_String(6, y, names[i], 0);

        switch (i)
        {
        case 0:  fmt_i16(b, (int16_t)JDG_OFFSET_MS, 3);      break;
        case 1:  fmt_i16(b, Render_NoteAlignPx, 2);          break;
        case 2:  fmt_i16(b, (int16_t)Play_SpeedPxPerS(), 3); break;
        default: fmt_i16(b, (int16_t)JDG_WIN_PERFECT, 2);    break;
        }
        RTLCD_String(42, y, b, 0);
    }

    RTLCD_String(0, 46, "dt avg", 0);
    fmt_i16(b, (int16_t)Judge_DtMeanMs(), 3);
    RTLCD_String(48, 46, b, 0);
    RTLCD_String(72, 46, "ms", 0);

    RTLCD_String(0, 55, "Whl:item S1- S2+", 0);
    RTLCD_Present();
}
