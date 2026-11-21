//============================================================================//
//  game_save.c - INFO D 设置持久化实现
//============================================================================//
#include <stdint.h>
#include "msp430.h"
#include "game_save.h"
#include "game_judge.h"
#include "game_play.h"
#include "game_render.h"
#include "game_touch.h"   /* Touch_SensPct */

#define SAVE_ADDR    0x1800u        /* INFO D 段起始 */
#define SAVE_MAGIC   0x5A5Au
#define SAVE_WORDS   16u            /* 固定 16 字（32 字节）；INFO D 有 64 字 */

/* 布局（16 字）：
     0      magic
     1      offMs(int16)
     2      alignPx(int16)
     3      speed
     4      winPerfect
     5      sens(%)
     6..8   曲目0 最高分(32 位 lo/hi) + 最高连击
     9..11  曲目1 同上
     12..14 曲目2 同上
     15     校验和(0..14 之和)
   ⚠ 每次扩布局旧记录都会校验不过，自动回落到默认值一次。 */

/* 历史最高分（按曲目 0..2）。上电时从 INFO D 读入，破纪录时立即回写。 */
static uint32_t bestScore[3] = {0, 0, 0};
static uint16_t bestCombo[3] = {0, 0, 0};
static uint8_t  newRecord = 0;

static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

static void apply_settings(int32_t off, int32_t align, int32_t spd, int32_t winP,
                           int32_t sens)
{
    JDG_OFFSET_MS      = clamp_i32(off,   -100, 100);
    Render_NoteAlignPx = (int16_t)clamp_i32(align, -2, 6);
    Play_SetSpeedPxPerS((uint16_t)clamp_i32(spd, 40, 120));
    JDG_WIN_PERFECT    = clamp_i32(winP, 15, 60);
    JDG_WIN_GREAT      = JDG_WIN_PERFECT * 2;
    JDG_WIN_GOOD       = JDG_WIN_PERFECT * 10 / 3;
    Touch_SensPct      = (uint16_t)clamp_i32(sens, 50, 250);
}

void Save_LoadSettings(void)
{
    const volatile uint16_t *p = (const volatile uint16_t *)SAVE_ADDR;
    uint16_t w[SAVE_WORDS];
    uint16_t sum = 0;
    uint8_t  i;

    for (i = 0; i < SAVE_WORDS; i++) w[i] = p[i];

    if (w[0] != SAVE_MAGIC) return;                  /* 未写过：沿用默认值 */

    /* ⚠ 校验范围必须和 build_words() 完全一致：
         求和 w[0..14]，与 w[15] 比较。
       这里曾经写成"只累加 0..5、和 w[6] 比"——那是 8 字布局时代的残留。
       布局扩到 16 字后 w[6] 已经是"曲目0 最高分低 16 位"，这个比较几乎必然失败，
       于是每次上电都判为"记录无效"，设置静默回落到源码默认值
       （最高分也一起丢），表现就是"怎么设都存不住"。 */
    for (i = 0; i < 15; i++) sum = (uint16_t)(sum + w[i]);
    if (sum != w[15]) return;                        /* 校验不过：沿用默认值 */

    apply_settings((int16_t)w[1], (int16_t)w[2], (int32_t)w[3], (int16_t)w[4],
                   (int16_t)w[5]);

    for (i = 0; i < 3; i++)
    {
        bestScore[i] = ((uint32_t)w[7 + i * 3] << 16) | (uint32_t)w[6 + i * 3];
        bestCombo[i] = w[8 + i * 3];
    }
    newRecord = 0;
}

/* 把当前设置 + 最高分打包成 16 个字 */
static void build_words(uint16_t *w)
{
    uint16_t i, sum = 0;

    w[0] = SAVE_MAGIC;
    w[1] = (uint16_t)JDG_OFFSET_MS;
    w[2] = (uint16_t)Render_NoteAlignPx;
    w[3] = Play_SpeedPxPerS();
    w[4] = (uint16_t)JDG_WIN_PERFECT;
    w[5] = Touch_SensPct;
    for (i = 0; i < 3; i++)
    {
        w[6 + i * 3] = (uint16_t)(bestScore[i] & 0xFFFFu);
        w[7 + i * 3] = (uint16_t)(bestScore[i] >> 16);
        w[8 + i * 3] = bestCombo[i];
    }
    for (i = 0; i < 15; i++) sum = (uint16_t)(sum + w[i]);
    w[15] = sum;
}

void Save_StoreSettings(void)
{
    uint16_t w[SAVE_WORDS];
    volatile uint16_t *p = (volatile uint16_t *)SAVE_ADDR;
    uint16_t gie = __get_SR_register() & GIE;
    uint8_t  i;

    build_words(w);

    /* 擦除+写入。关中断是必要的（避免 ISR 在写模式下取指）；
       本次约数毫秒，只发生在退出设置界面 / 破纪录结算时，不在实时路径上。
       副作用：期间可能漏掉 1~2 次 TA0 溢出，时间基产生约 2.6 ms 偏移
       —— 对"求差"的用法（dt、帧长）无影响。 */

    __disable_interrupt();
    FCTL3 = FWKEY;                        /* 解锁 */
    FCTL1 = FWKEY + ERASE;                /* 擦除模式 */
    *p = 0;                               /* 空写触发整段擦除 */
    while (FCTL3 & BUSY) ;                /* 等擦除完成（INFO 段约 1~3 ms） */
    FCTL1 = FWKEY + WRT;                  /* 写入模式 */
    for (i = 0; i < SAVE_WORDS; i++)
    {
        while (FCTL3 & BUSY) ;            /* 每个字写完再写下一个 */
        p[i] = w[i];
    }
    while (FCTL3 & BUSY) ;
    FCTL1 = FWKEY;                        /* 退出写模式 */
    FCTL3 = FWKEY + LOCK;                 /* 上锁 */
    __bis_SR_register(gie);
}

uint32_t Save_BestScore(uint8_t chart) { return (chart < 3) ? bestScore[chart] : 0; }
uint16_t Save_BestCombo(uint8_t chart) { return (chart < 3) ? bestCombo[chart] : 0; }

/* 结算时调用：破了纪录就更新并立刻落盘（约数 ms，结算页没有实时要求）。 */
uint8_t Save_TrySetRecord(uint8_t chart, uint32_t score, uint16_t combo)
{
    if (chart >= 3) return 0;
    if (score > bestScore[chart] ||
        (score == bestScore[chart] && combo > bestCombo[chart]))
    {
        bestScore[chart] = score;
        bestCombo[chart] = combo;
        newRecord = 1;
        Save_StoreSettings();
        return 1;
    }
    return 0;
}

/* 清空全部曲目的历史最高分并落盘（设置页 CLR 项调用） */
void Save_ClearRecords(void)
{
    uint8_t i;
    for (i = 0; i < 3; i++) { bestScore[i] = 0; bestCombo[i] = 0; }
    newRecord = 0;
    Save_StoreSettings();
}

void    Save_ClearNewRecord(void) { newRecord = 0; }
uint8_t Save_NewRecord(void)      { return newRecord; }
