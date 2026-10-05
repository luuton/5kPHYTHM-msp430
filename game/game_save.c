//============================================================================//
//  game_save.c - INFO D 设置持久化实现
//============================================================================//
#include <stdint.h>
#include "msp430.h"
#include "game_save.h"
#include "game_judge.h"
#include "game_play.h"
#include "game_render.h"

#define SAVE_ADDR    0x1800u        /* INFO D 段起始 */
#define SAVE_MAGIC   0x5A5Au
#define SAVE_WORDS   8u             /* 固定 8 字（16 字节）布局 */

/* 布局：0=magic 1=offMs(int16) 2=alignPx(int16) 3=speed 4=winPerfect
         5=校验和(0..4 之和) 6,7=保留(0xFFFF) */

static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

static void apply_settings(int32_t off, int32_t align, int32_t spd, int32_t winP)
{
    JDG_OFFSET_MS      = clamp_i32(off,   -100, 100);
    Render_NoteAlignPx = (int16_t)clamp_i32(align, -2, 6);
    Play_SetSpeedPxPerS((uint16_t)clamp_i32(spd, 40, 120));
    JDG_WIN_PERFECT    = clamp_i32(winP, 15, 60);
    JDG_WIN_GREAT      = JDG_WIN_PERFECT * 2;
    JDG_WIN_GOOD       = JDG_WIN_PERFECT * 10 / 3;
}

void Save_LoadSettings(void)
{
    const volatile uint16_t *p = (const volatile uint16_t *)SAVE_ADDR;
    uint16_t w[SAVE_WORDS];
    uint16_t sum = 0;
    uint8_t  i;

    for (i = 0; i < SAVE_WORDS; i++) w[i] = p[i];

    if (w[0] != SAVE_MAGIC) return;                  /* 未写过：沿用默认值 */
    for (i = 0; i < 5; i++) sum = (uint16_t)(sum + w[i]);
    if (sum != w[5]) return;                         /* 校验不过：沿用默认值 */

    apply_settings((int16_t)w[1], (int16_t)w[2], (int32_t)w[3], (int16_t)w[4]);
}

void Save_StoreSettings(void)
{
    volatile uint16_t *p = (volatile uint16_t *)SAVE_ADDR;
    uint16_t w[SAVE_WORDS];
    uint16_t gie = __get_SR_register() & GIE;
    uint8_t  i;

    w[0] = SAVE_MAGIC;
    w[1] = (uint16_t)JDG_OFFSET_MS;
    w[2] = (uint16_t)Render_NoteAlignPx;
    w[3] = Play_SpeedPxPerS();
    w[4] = (uint16_t)JDG_WIN_PERFECT;
    w[5] = 0;
    for (i = 0; i < 5; i++) w[5] = (uint16_t)(w[5] + w[i]);
    w[6] = 0xFFFFu;
    w[7] = 0xFFFFu;

    /* 擦除+写入。关中断是必要的（避免 ISR 在写模式下取指）；
       本次约数毫秒，且只发生在退出设置界面时，不在实时路径上。
       副作用：期间可能漏掉 1~2 次 TA0 溢出，使时间基产生一个恒定的
       约 2.6 ms 偏移 —— 对"求差"的用法（dt、帧长）无影响。 */
    __disable_interrupt();
    FCTL3 = FWKEY;                        /* 解锁 */
    FCTL1 = FWKEY + ERASE;                /* 擦除模式 */
    *p = 0;                               /* 空写触发整段擦除 */
    FCTL1 = FWKEY + WRT;                  /* 写入模式 */
    for (i = 0; i < SAVE_WORDS; i++) p[i] = w[i];
    FCTL1 = FWKEY;                        /* 退出写模式 */
    FCTL3 = FWKEY + LOCK;                 /* 上锁 */
    __bis_SR_register(gie);
}
