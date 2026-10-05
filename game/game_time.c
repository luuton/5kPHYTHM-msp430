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

void Time_Init(void)
{
    TA0CTL = TACLR;                     /* TAR 清零 */
    TA0CTL = TASSEL_2 + MC_2 + TAIE;    /* SMCLK、连续模式、溢出中断允许 */
}

void Time_Deinit(void)
{
    TA0CTL = 0;
    time_hi = 0;
}

#pragma vector = TIMER0_A1_VECTOR
__interrupt void TIME_TA0_Overflow_ISR(void)
{
    switch (__even_in_range(TA0IV, TA0IV_TA0IFG))
    {
    case TA0IV_TA0IFG:
        time_hi++;
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
        /* 硬件已溢出但 ISR 被我们挡住了：这次溢出尚未计入 time_hi，自行补 1 */
        hi++;
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

/* 单位 ms：1 ms = 25000 tick，回绕周期约 47.7 小时 */
uint32_t Time_NowMs(void)
{
    return time_ticks() / 25000u;
}
