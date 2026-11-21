//============================================================================//
//  game_time.c - Timer_A0 40 ns 时基实现
//
//  Timer_A0 在原工程中完全空闲（HAL 只用 Timer_B0 做背光 PWM，CTS 用 Timer_A1）。
//  溢出走 TIMER0_A1_VECTOR（TA0IFG 通道）。
//
//  ⚠ 读数的原子性：早期版本用 "读 hi → 读 TA0R → 复读 hi" 的软同步，存在漏洞——
//  若硬件刚溢出（TAIFG 置位）而溢出 ISR 尚未执行，time_hi 仍是旧值、TA0R 已回绕，
//  读数会比真实值小 65536 tick(2.62 ms)，两次相减出现负值 → 无符号回绕成 4.29 s。
//  实测表现为 CTS 扫描耗时偶发 25 ms 尖峰（2^32 ns / 扫描次数 ≈ 25039 µs）。
//  现在改为关中断的临界区 + TAIFG 补偿，彻底消除。
//============================================================================//
#include "msp430.h"
#include "game_time.h"

volatile uint16_t time_hi = 0;

/* ⚠ 毫秒计时必须独立于 32 位 tick 计数。
   32 位 tick = 2^32 x 40 ns = 171.8 s 就回绕一次；直接用 ticks/25000 当毫秒，
   每到 171.8 s 毫秒值就会倒退 171798 ms。实测后果：
     · 帧耗时算出 4294795512 ms（= 2^32 - 171784），打出 `!` 长帧标记
     · songNow = nowMs - startWallMs 变成巨大值 → 所有音符瞬间判 MISS、
       轨道清空（"铺面消失"），随后状态机再也回不来（表现为卡死）
   这里改成：溢出中断里维护 32 位毫秒累加器（回绕周期 49.7 天）。
   每个溢出 65536 tick = 2.62144 ms，小数部分留在 ms_frac 里累计。 */
volatile uint32_t time_ms   = 0;
static   volatile uint32_t ms_frac = 0;     /* 毫秒小数部分：tick，< 25000 */

void Time_Init(void)
{
    time_ms   = 0;
    ms_frac   = 0;
    time_hi   = 0;
    TA0CTL = TACLR;                     /* TAR 清零 */
    TA0CTL = TASSEL_2 + MC_2 + TAIE;    /* SMCLK、连续模式、溢出中断允许 */
}

void Time_Deinit(void)
{
    TA0CTL = 0;
    time_hi = 0;
    time_ms = 0;
    ms_frac = 0;
}

#pragma vector = TIMER0_A1_VECTOR
__interrupt void TIME_TA0_Overflow_ISR(void)
{
    switch (__even_in_range(TA0IV, TA0IV_TA0IFG))
    {
    case TA0IV_TA0IFG:
        time_hi++;
        {   /* 毫秒累加：65536 tick = 2.62144 ms，整数部分进位，小数留到下次 */
            uint32_t f = ms_frac + 65536u;
            time_ms += f / 25000u;
            ms_frac  = f % 25000u;
        }
        break;
    default:
        break;
    }
}

/* 原子读 32 位 tick。关中断很短暂（约 10 周期），两个调用点都在主循环。 */
static uint32_t time_ticks(void)
{
    uint16_t gie = __get_SR_register() & GIE;
    uint16_t hi, lo;

    __disable_interrupt();
    hi = time_hi;
    lo = TA0R;
    if (TA0CTL & TAIFG)
    {
        /* 有尚未处理的溢出（ISR 被本临界区挡住了）。TAIFG 只说"发生过溢出"，
           不说明它相对上一行 lo 的读取是先后 —— 必须用 lo 的数值判别：
             临界区只有几微秒，远短于 TA0 回绕周期 2.62 ms，
             所以这个溢出必然是刚刚发生的，lo 只会落在两端：
               lo 接近 0xFFFF → 读到的是溢出前的值 → 快照取 (hi, lo)，不加 1
               lo 接近 0x0000 → 读到的是溢出后的值 → hi 需要 +1
           （早期版本无条件 +1，在第一种情况下让读数偏大 65536 tick = 2621 us，
             实测表现为扫描耗时偶发 -1313 us 回绕成 4.29e9、打印成 64228。） */
        if (lo < 0x8000u)
        {
            hi++;
            /* ⚠ 必须同时撤掉这个溢出标志。
               这次溢出的高位已经由本函数补记，如果不清 TAIFG，退出临界区后
               挂起的定时器中断还会再执行一次 time_hi++，32 位计时被多加
               65536 tick（2.62 ms）。实测症状：帧耗时统计里反复出现
               26 / 29 / 30 ms 这种"16 + k x 2.62"的值（k=4..5），
               而真实帧率是 60 fps；音符位置每帧因此多跳十几毫秒，
               目押精度被凭空拉低。 */
            TA0CTL &= (uint16_t)~TAIFG;
        }
    }
    __bis_SR_register(gie);

    return (((uint32_t)hi << 16) | lo);
}

/* 单位 ns。注意：tick*40 在 uint32 下每 4.29 s 回绕一次，
   因此本函数只适合做"短间隔求差"（差值在 mod 2^32 意义下仍正确），
   不要用它的绝对值做长时间累积。 */
uint32_t Time_Now(void)
{
    return time_ticks() * 40u;
}

/* 单位 µs：1 µs = 25 tick。比 ns 便宜（除法常量优化为移位乘），
   且回绕周期 171 s，适合扫描耗时这类测量。 */
uint32_t Time_NowUs(void)
{
    return time_ticks() / 25u;
}

/* 单位 ms：来自独立毫秒累加器，回绕周期 49.7 天（不再是 171.8 s）。
   读法：临界区内一次取 (time_ms, TA0R)，若正好处在"溢出已发生但 ISR 未执行"
   的窗口内，用 TAIFG + lo 判别并补上本次溢出的份额（与 time_ticks 同样的技巧，
   区别是这里只补偿本地结果、不改全局状态，ISR 随后照常累加，不会重复计数）。 */
uint32_t Time_NowMs(void)
{
    uint16_t gie = __get_SR_register() & GIE;
    uint32_t ms;
    uint16_t lo;

    __disable_interrupt();
    ms = time_ms;
    lo = TA0R;
    if ((TA0CTL & TAIFG) && (lo < 0x8000u))
    {
        uint32_t f = ms_frac + 65536u;
        ms += f / 25000u;
    }
    __bis_SR_register(gie);

    return ms + (uint32_t)lo / 25000u;
}
