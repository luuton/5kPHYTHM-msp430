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

/* 字符常量宏。本项目多次因多层转义把
   转义序列 写成真实的换行字符，
   导致编译器报 "missing closing quote"。改用数值常量后不再依赖转义。 */
#define CH_CR   13
#define CH_LF   10

#include "game_judge.h"   /* 横幅要输出 JDG_OFFSET_MS */
#include "game_touch.h"    /* 横幅要输出 Touch_SensPct */
#include "game_usb.h"     /* 诊断行要读 USB 寄存器快照 */

/* --------------------------------------------------------------------------
   输出层：两种后端的唯一差别就在下面这几个函数里，格式代码完全共用。
   -------------------------------------------------------------------------- */
#if TELEM_BACKEND_USB

/* USB 模式：只往行缓冲里拼字符，遇到 CH_LF 才整行交给 USB。
   这样做是因为 USBCDC_sendData 一次最多 64 字节，逐字符调用既慢又必然失败；
   而行缓冲是纯内存操作，几个周期一字符，对扫描窗口毫无影响。 */
/* ⚠ 必须大于最长的一行（当前最长是诊断行的 89 字符）。
   写成 80 时 D 行会在 U7=... 处被截断，连结尾的 CR/LF 一起丢掉，
   于是下一条 F 行被拼到同一条里（实测日志里出现过
   "D C=0007 I=00C4 P=0E48F  28 1322 ..." 这种粘连）。 */
#define LINE_MAX  128

static char    line[LINE_MAX];
static uint8_t ln = 0;

static void put(char c)
{
    if (c == CH_LF)
    {
        if (ln < LINE_MAX) line[ln++] = CH_LF;
        if (Usb_IsReady()) Usb_Write((const uint8_t *)line, ln);
        ln = 0;                              /* 未就绪则整行丢弃 */
        return;
    }
    if (ln < LINE_MAX - 1) line[ln++] = c;   /* 超长截断，仍在 CH_LF 时提交 */
}

static void telem_backend_init(void)
{
    /* USB 由 Usb_Init() 在总装处初始化；此处无需动作 */
    ln = 0;
}

#else   /* ---------------- UART 后端（USCI_A1, P4.4/P4.5, 115200） ---------- */

static void put(char c)
{
    while (!(UCA1IFG & UCTXIFG)) ;
    UCA1TXBUF = (uint8_t)c;
}

static void telem_backend_init(void)
{
    P4SEL |= BIT4 | BIT5;
    UCA1CTL1 |= UCSWRST;
    UCA1CTL0  = 0x00;
    UCA1CTL1  = UCSSEL_2 + UCSWRST;
    UCA1BR0   = 13;
    UCA1BR1   = 0;
    UCA1MCTL  = UCBRF_9 + UCBRS_0 + UCOS16;
    UCA1CTL1 &= ~UCSWRST;
}

#endif


#define TELEM_QMAX  8

typedef struct {
    uint32_t tapMs;
    uint32_t noteMs;
    uint8_t  lane;
    uint8_t  res;
    uint16_t d[5];  /* 击打瞬间五路 delta（诊断串键用） */
} TelemEv;

static TelemEv  q[TELEM_QMAX];
static uint8_t  qn = 0;         /* 待发条数 */
static uint8_t  qdrop = 0;      /* 队列满丢弃计数 */
static uint8_t  enabled = 1;
static uint16_t tapSeq = 0;

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
    telem_backend_init();
    qn = 0;
    qdrop = 0;
    enabled = 1;
}

void Telem_Enable(uint8_t on) { enabled = on; }

/* 开机横幅：V <tag> spd=<n> aln=<±n> win=<p>/<g>/<d> thr=<t0>,<t1>,... */
/* ---- 版本横幅：USB 模式下可能尚未枚举，需挂起后补发 ----
   顺带把一个坑记在这里：USB CDC 全速端点单包最多 64 字节，
   早期把 thr 五项塞在同一行（共 73 字符）会被截断在第二个包上。
   现在拆成 V（参数）与 K（阈值）两行，各自都在 64 字节以内。 */
static const char     *verTag = 0;
static uint16_t        verSpd = 0;
static int16_t         verAln = 0;
static int32_t         verWp = 0, verWg = 0, verWd = 0;
static const uint16_t *verThr = 0;
#if TELEM_BACKEND_USB
static uint8_t         verPending = 0;   /* 仅在 USB 后端使用：未枚举时挂起横幅 */
#endif
#if TELEM_BACKEND_USB
static uint8_t         wasUsbReady = 0;
#endif

static void version_output(void)
{
    uint8_t i;

    if (!enabled || !verTag) return;

    put('V'); put(' ');
    puts_raw(verTag);
    puts_raw(" spd="); put_u32w(verSpd, 2);
    puts_raw(" off=");
    if (JDG_OFFSET_MS < 0) { put('-'); put_u32w((uint32_t)(-JDG_OFFSET_MS), 3); }
    else                   { put('+'); put_u32w((uint32_t)JDG_OFFSET_MS, 3); }
    puts_raw(" aln=");
    if (verAln < 0) { put('-'); put_u32w((uint32_t)(-verAln), 2); }
    else            { put('+'); put_u32w((uint32_t)verAln, 2); }
    puts_raw(" win=");
    put_u32w((uint32_t)verWp, 2); put('/');
    put_u32w((uint32_t)verWg, 2); put('/');
    put_u32w((uint32_t)verWd, 3);
    put(CH_CR); put(CH_LF);

    put('K'); puts_raw(" thr=");
    for (i = 0; i < 5; i++)
    {
        if (i) put(',');
        put_u32w(verThr[i], 3);
    }
        puts_raw(" sens="); put_u32w(Touch_SensPct, 3);
    put(CH_CR); put(CH_LF);
}

void Telem_Version(const char *tag, uint16_t speedPx, int16_t alignPx,
                   int32_t winPerfect, int32_t winGreat, int32_t winGood,
                   const uint16_t *thr5)
{
    if (!enabled) return;

    verTag = tag;
    verSpd = speedPx;
    verAln = alignPx;
    verWp  = winPerfect;
    verWg  = winGreat;
    verWd  = winGood;
    verThr = thr5;

#if TELEM_BACKEND_USB
    verPending = 1;
    if (!Usb_IsReady()) return;      /* 挂起，等 Telem_Flush 检测到 ready 再补发 */
#endif
    version_output();
#if TELEM_BACKEND_USB
    verPending = 0;
#endif
}

static const char *const RES_NAME[6] = { "-", "MISS", "GOOD", "GREAT", "PERFECT", "WRONG" };

/* ---- 入队（不阻塞，可在扫描窗口内安全调用） ---- */
void Telem_Tap(uint8_t lane, uint32_t tapMs, uint32_t noteMs, uint8_t result,
               const uint16_t *delta)
{
    uint8_t i;

    if (!enabled) return;
    if (qn >= TELEM_QMAX) { qdrop++; return; }
    q[qn].lane   = lane;
    q[qn].tapMs  = tapMs;
    q[qn].noteMs = noteMs;
    q[qn].res    = result;
    for (i = 0; i < 5; i++) q[qn].d[i] = delta ? delta[i] : 0;
    qn++;
}

/* ---- 出队发送（扫描窗口外调用） ---- */
void Telem_Flush(void)
{
    uint8_t i;

    if (!enabled) { qn = 0; return; }

#if TELEM_BACKEND_USB
    /* 检测"刚连上"（含拔插后重连），补发开关机横幅 —— 否则开机时
       电脑还没打开串口，V/K 行会永远丢失，也就失去了"日志可追溯"的意义。 */
    {
        uint8_t r = Usb_IsReady();
        if (r && !wasUsbReady && verPending)
        {
            version_output();
            verPending = 0;
        }
        wasUsbReady = r;
    }
#endif

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
        /* 击打瞬间的五路 delta：用来定位"串键"（相邻路被一起触发） */
        {
            uint8_t k;
            puts_raw(" |");
            for (k = 0; k < 5; k++) { put(' '); put_u32w(q[i].d[k], 4); }
        }
        put(CH_CR); put(CH_LF);
    }
    qn = 0;

    if (qdrop)
    {
        puts_raw("! dropped ");
        put_u32w(qdrop, 2);
        put(CH_CR); put(CH_LF);
        qdrop = 0;
    }
}

void Telem_FrameStats(uint16_t worstFrameMs, uint16_t bodyMaxMs,
                      uint16_t renderMaxUs,
                      uint16_t scanMinUs, uint16_t scanAvgUs, uint16_t scanMaxUs,
                      uint16_t drawMaxUs, uint16_t presentMaxUs)
{
    if (!enabled) return;
    put('F'); put(' ');
    put_u32w(worstFrameMs, 3);
    put(' ');  put_u32w(scanMinUs, 4);
    put(' ');  put_u32w(scanAvgUs, 4);
    put(' ');  put_u32w(scanMaxUs, 4);
    put(' ');  put_u32w(bodyMaxMs, 3);      /* 帧体最长 ms */
    put(' ');  put_u32w(renderMaxUs, 5);    /* 渲染最长 us */
    put(' ');  put_u32w(drawMaxUs, 5);
    put(' ');  put_u32w(presentMaxUs, 5);
    put(CH_CR); put(CH_LF);
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
    put(CH_CR); put(CH_LF);
}

/* 标定行：当前 delta + 峰值保持，便于用串口精确读值（P3 用）
   C   48   12    3    0    5  |  312  288  155   94  120
     ↑ 当前 5 路 delta          ↑ 上电以来峰值（S1 清） */
/* 标定行。格式压到 62 字节以内 —— USB CDC 全速端点单包最多 64 字节，
   原来的 "C  48  12   3   0   5 | 312 ... | thr ..." 有 86 字节，
   会被截断在第二个包上（USB 后端下表现为半行乱码）。 */
/* USB 诊断行：把 LCD 诊断页上的内容同步打一份到串口，
   便于在 PC 上记录/对比多次枚举过程（LCD 只能瞬读，无法留存）。
   格式：D C=裁剪 I=中断使能 P=电源 F=标志 E=枚举状态 S=挂起 N=ISR次数
         V=最后事件 X=软件补的RESET SP=软件补的SETUP PL=USBPLLCTL U7=UCSCTL7 */
/* delta 区间：dmin[i] / dmax[i] 是这一秒内第 i 路 delta 的最小/最大值。
   判读方法：
     · 某路 min 长期贴着阈值 → 卡键（基线漂移），需要重标定
     · 某路 min 很低而 max 偶尔冲高 → 正常噪声，抬阈值即可
   第一行 M 是 min，第二行 X 是 max。 */
void Telem_DeltaRange(const uint16_t *dmin, const uint16_t *dmax)
{
    uint8_t i;
    if (!enabled) return;

    put('M');
    for (i = 0; i < 5; i++) { put(' '); put_u32w(dmin[i], 4); }
    put(CH_CR); put(CH_LF);

    put('X');
    for (i = 0; i < 5; i++) { put(' '); put_u32w(dmax[i], 4); }
    put(CH_CR); put(CH_LF);
}

/* 长帧标记：帧耗时超过 500 ms 时立刻打一行，记录耗时和当时的绝对时间。
   用途：区分"真的卡了 25 秒"和"毫秒计数器跳变"——
   如果 t 只前进了 25 秒而后续帧正常，说明这段时间主循环确实没跑
   （例如调试器暂停了目标）；如果 t 的增量与耗时对不上，则是计时本身出了问题。 */
void Telem_Stall(uint32_t frameMs, uint32_t nowMs)
{
    if (!enabled) return;
    put('!');
    put(' '); put_u32w(frameMs, 6);
    put(' '); put_u32w(nowMs, 8);
    put(CH_CR); put(CH_LF);
}

void Telem_Play(uint8_t state, uint32_t songMs, uint16_t head, uint16_t count)
{
    if (!enabled) return;
    put('P');
    put(' '); put_u32w(state, 1);
    put(' '); put_u32w(songMs, 6);
    put(' '); put_u32w(head, 4);
    put(' '); put_u32w(count, 4);
    put(CH_CR); put(CH_LF);
}

void Telem_Clock(uint32_t gameSec, uint8_t rtcSec)
{
    if (!enabled) return;
    put('S');
    put(' '); put_u32w(gameSec, 5);
    put(' '); put_u32w(rtcSec, 2);
    put(CH_CR); put(CH_LF);
}

void Telem_Calib(const uint16_t *delta, const uint16_t *peak, const uint16_t *thr)
{
    uint8_t i;
    if (!enabled) return;

    put('C'); put(' ');
    for (i = 0; i < 5; i++)
    {
        if (i) put(',');
        put_u32w(delta[i], 3);
    }
    put('|');
    for (i = 0; i < 5; i++)
    {
        if (i) put(',');
        put_u32w(peak[i], 3);
    }
    put('|');
    for (i = 0; i < 5; i++)
    {
        if (i) put(',');
        put_u32w(thr[i], 3);
    }
    put(CH_CR); put(CH_LF);
}
