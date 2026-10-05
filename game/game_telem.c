//============================================================================//
//  game_telem.c - 串口遥测实现
//
//  输出格式（ASCII，PC 端任意串口助手可直接读，也可 grep）：
//
//    T 00123 2    1240    1210  +30 PERFECT   # 击打: 序号 lane note_ms tap_ms dt_ms 判定
//    T 00124 0     ---    1300   --- WRONG    # 空按（该轨窗口内无音符）
//    F  16  380  420  610                     # 帧: worst_ms scan_min scan_avg scan_max (us)
//    R   88   12    3    5   4321  45         # 结算: PERFECT GREAT GOOD MISS score maxcombo
//
//  波特率 115200 @ SMCLK 25 MHz（UCOS16: UCBR=13, UCBRF=9，误差 +0.06%）
//  引脚 P4.4 = UCA1TXD, P4.5 = UCA1RXD
//
//  ⚠ 阻塞策略：一行 40 字节 @115200 ≈ 3.5 ms。若在"输入扫描窗口"内直接发送，
//  连打（和弦/快速连续击打）会阻塞窗口、减少扫描次数、拉低输入采样率。
//  因此击打事件先入队，由主循环在扫描窗口结束后调 Telem_Flush() 统一发出。
//============================================================================//
#include <stdint.h>
#include "msp430.h"
#include "game_telem.h"

#define TELEM_QMAX  8

typedef struct {
    uint32_t tapMs;
    uint32_t noteMs;
    uint8_t  lane;
    uint8_t  res;
} TelemEv;

static TelemEv  q[TELEM_QMAX];
static uint8_t  qn = 0;         /* 待发条数 */
static uint8_t  qdrop = 0;      /* 队列满丢弃计数 */
static uint8_t  enabled = 1;
static uint16_t tapSeq = 0;

static void put(char c)
{
    while (!(UCA1IFG & UCTXIFG)) ;
    UCA1TXBUF = (uint8_t)c;
}

static void puts_raw(const char *s)
{
    while (*s) put(*s++);
}

/* 无符号十进制，右对齐宽度 w（前导空格） */
static void put_u32w(uint32_t v, uint8_t w)
{
    char b[11];
    int8_t i = 10;
    uint8_t n;

    b[10] = 0;
    if (v == 0) b[--i] = '0';
    while (v && i > 0) { b[--i] = (char)('0' + (v % 10)); v /= 10; }
    n = (uint8_t)(10 - i);
    while (n < w) { put(' '); w--; }
    while (b[i]) put(b[i++]);
}

/* 固定 5 字符：前导空格 + 显式正负号 + 3 位数字，例如 " - 79" / " + 13"。
   早期版本在负数时把 '-' 直接贴到前一个字段的数字上（"1121-  79"），无法分词。 */
static void put_i32(int32_t v, uint8_t w)
{
    (void)w;
    put(' ');
    if (v < 0) { put('-'); v = -v; } else { put('+'); }
    put_u32w((uint32_t)v, 3);
}

void Telem_Init(void)
{
    P4SEL |= BIT4 | BIT5;                 /* P4.4 TXD, P4.5 RXD */
    UCA1CTL1 |= UCSWRST;
    UCA1CTL0  = 0x00;                     /* 8N1，异步 */
    UCA1CTL1  = UCSSEL_2 + UCSWRST;       /* SMCLK */
    UCA1BR0   = 13;                       /* N/16 = 25e6/115200/16 = 13.56 */
    UCA1BR1   = 0;
    UCA1MCTL  = UCBRF_9 + UCBRS_0 + UCOS16;
    UCA1CTL1 &= ~UCSWRST;
    qn = 0;
    qdrop = 0;
    enabled = 1;
}

void Telem_Enable(uint8_t on) { enabled = on; }

static const char *const RES_NAME[6] = { "-", "MISS", "GOOD", "GREAT", "PERFECT", "WRONG" };

/* ---- 入队（不阻塞，可在扫描窗口内安全调用） ---- */
void Telem_Tap(uint8_t lane, uint32_t tapMs, uint32_t noteMs, uint8_t result)
{
    if (!enabled) return;
    if (qn >= TELEM_QMAX) { qdrop++; return; }
    q[qn].lane   = lane;
    q[qn].tapMs  = tapMs;
    q[qn].noteMs = noteMs;
    q[qn].res    = result;
    qn++;
}

/* ---- 出队发送（扫描窗口外调用） ---- */
void Telem_Flush(void)
{
    uint8_t i;

    if (!enabled) { qn = 0; return; }

    for (i = 0; i < qn; i++)
    {
        put('T'); put(' ');
        put_u32w(++tapSeq, 5);
        put(' '); put_u32w(q[i].lane, 1);

        if (q[i].noteMs == 0xFFFFFFFFu)
        {
            puts_raw("     ---");                       /* note = --- */
            put(' '); put_u32w(q[i].tapMs, 7);
            puts_raw("   ---");                         /* dt   = --- */
            puts_raw(" WRONG");
        }
        else
        {
            put(' '); put_u32w(q[i].noteMs, 7);
            put(' '); put_u32w(q[i].tapMs, 7);
            put_i32((int32_t)((int32_t)q[i].tapMs - (int32_t)q[i].noteMs), 5);
            put(' '); puts_raw(RES_NAME[q[i].res & 7]);
        }
        put('\r'); put('\n');
    }
    qn = 0;

    if (qdrop)
    {
        puts_raw("! dropped ");
        put_u32w(qdrop, 2);
        put('\r'); put('\n');
        qdrop = 0;
    }
}

void Telem_FrameStats(uint16_t worstFrameMs,
                      uint16_t scanMinUs, uint16_t scanAvgUs, uint16_t scanMaxUs)
{
    if (!enabled) return;
    put('F'); put(' ');
    put_u32w(worstFrameMs, 3);
    put(' ');  put_u32w(scanMinUs, 4);
    put(' ');  put_u32w(scanAvgUs, 4);
    put(' ');  put_u32w(scanMaxUs, 4);
    put('\r'); put('\n');
}

void Telem_Result(uint16_t pf, uint16_t gr, uint16_t gd, uint16_t ms,
                  uint32_t score, uint16_t maxCombo)
{
    if (!enabled) return;
    put('R'); put(' ');
    put_u32w(pf, 4); put(' ');
    put_u32w(gr, 4); put(' ');
    put_u32w(gd, 4); put(' ');
    put_u32w(ms, 3); put(' ');
    put_u32w(score, 6); put(' ');
    put_u32w(maxCombo, 3);
    put('\r'); put('\n');
}

/* 标定行：当前 delta + 峰值保持，便于用串口精确读值（P3 用）
   C   48   12    3    0    5  |  312  288  155   94  120
     ↑ 当前 5 路 delta          ↑ 上电以来峰值（S1 清） */
void Telem_Calib(const uint16_t *delta, const uint16_t *peak, const uint16_t *thr)
{
    uint8_t i;
    if (!enabled) return;
    put('C');
    for (i = 0; i < 5; i++) { put(' '); put_u32w(delta[i], 4); }
    puts_raw(" |");
    for (i = 0; i < 5; i++) { put(' '); put_u32w(peak[i], 4); }
    puts_raw(" | thr");
    for (i = 0; i < 5; i++) { put(' '); put_u32w(thr[i], 4); }
    put('\r'); put('\n');
}
