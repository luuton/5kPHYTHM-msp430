//============================================================================//
//  game_rtlcd.c - 影子缓冲 + 批量 flush 实现
//
//  与 TI HAL 的关系：HAL 是写穿式，且它内部维护 dogs102x6Memory[]（显存镜像
//  前面带 2 字节头）作为唯一事实来源。本模块不绕开它：
//    · 绘制：只写本模块自己的 shadow[8][102]
//    · 呈现：对每页执行 setAddress(p,0) + writeData(page,102)
//  writeData 的批量路径恰好会把数据同步写回 dogs102x6Memory（HAL 的镜像跟着
//  更新，保持一致），并且一次事务推 102 B，比 HAL 的写穿像素原语快一个数量级。
//
//  帧成本实测预算（project.md §5.2）：102 B 批量 ≈ 3.4 µs/B → 8 页 ≈ 2.7 ms。
//============================================================================//
#include <stdint.h>
#include "msp430.h"
#include "HAL_Dogs102x6.h"
#include "game_rtlcd.h"

static uint8_t shadow[RTLCD_PAGES][RTLCD_W];   /* 816 B RAM */

/* 6x8 字体（从 HAL 复制，0x20-0x7E；HAL 里的是 static 无法直接引用） */
static const uint8_t FONT6x8_SH[] = {
#include "game_font6x8.h"
};

#define GLYPH(ch)  (&FONT6x8_SH[(uint16_t)((ch) - 0x20) * 6])

void RTLCD_Init(void)
{
    Dogs102x6_clearScreen();      /* 真屏清零（顺便校准 HAL 指针） */
    RTLCD_Clear();
}

void RTLCD_Clear(void)
{
    uint8_t p, c;
    for (p = 0; p < RTLCD_PAGES; p++)
        for (c = 0; c < RTLCD_W; c++)
            shadow[p][c] = 0;
}

void RTLCD_PresentPages(uint8_t mask)
{
    uint8_t p;
    for (p = 0; p < RTLCD_PAGES; p++)
    {
        if (mask & (1u << p))
        {
            Dogs102x6_setAddress(p, 0);
            Dogs102x6_writeData(shadow[p], 102);
        }
    }
}

void RTLCD_Present(void)
{
    RTLCD_PresentPages(0xFF);
}

void RTLCD_Pixel(uint8_t x, uint8_t y, uint8_t on)
{
    uint8_t m;
    if (x >= RTLCD_W || y >= RTLCD_H) return;
    /* 与 HAL pixelDraw 一致：page 内 y%8==0 → 0x80（MSB 在上） */
    m = (uint8_t)(0x80u >> (y & 7));
    if (on) shadow[y >> 3][x] |= m;
    else    shadow[y >> 3][x] &= (uint8_t)~m;
}

void RTLCD_FillRect(uint8_t x1, uint8_t y1, uint8_t x2, uint8_t y2, uint8_t on)
{
    uint8_t x, y, t;
    if (x1 > x2) { t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { t = y1; y1 = y2; y2 = t; }
    if (x2 >= RTLCD_W) x2 = RTLCD_W - 1;
    if (y2 >= RTLCD_H) y2 = RTLCD_H - 1;
    if (x1 >= RTLCD_W || y1 >= RTLCD_H) return;

    if (on)
    {
        for (y = y1; y <= y2; y++)
        {
            uint8_t m = (uint8_t)(0x80u >> (y & 7));
            for (x = x1; x <= x2; x++) shadow[y >> 3][x] |= m;
        }
    }
    else
    {
        for (y = y1; y <= y2; y++)
        {
            uint8_t m = (uint8_t)(0x80u >> (y & 7));
            for (x = x1; x <= x2; x++) shadow[y >> 3][x] &= (uint8_t)~m;
        }
    }
}

void RTLCD_HLine(uint8_t x1, uint8_t x2, uint8_t y, uint8_t on)
{
    RTLCD_FillRect(x1, y, x2, y, on);
}

static void blit_col(uint8_t x, uint8_t y, uint8_t col, uint8_t inv)
{
    /* FONT 列字节 MSB = 列顶像素，与 page 位序（MSB=top）一致：
       y 页内对齐（y&7==0）时整字节直接落；跨页时拆高低两半。 */
    uint8_t p, off;

    if (x >= RTLCD_W || y >= RTLCD_H) return;
    if (inv) col = (uint8_t)~col;

    p   = (uint8_t)(y >> 3);
    off = (uint8_t)(y & 7);
    if (off == 0)
    {
        shadow[p][x] |= col;
        return;
    }
    shadow[p][x]     |= (uint8_t)(col >> off);
    if (p + 1 < RTLCD_PAGES)
        shadow[p + 1][x] |= (uint8_t)(col << (8 - off));
}

uint8_t RTLCD_Char(uint8_t x, uint8_t y, char ch, uint8_t inv)
{
    const uint8_t *g;
    uint8_t c;
    if ((uint8_t)ch < 0x20 || (uint8_t)ch > 0x7E) ch = '?';
    g = GLYPH((uint8_t)ch);
    for (c = 0; c < 6; c++)
        blit_col(x + c, y, g[c], inv);
    return 6;
}

void RTLCD_String(uint8_t x, uint8_t y, const char *s, uint8_t inv)
{
    while (*s) x += RTLCD_Char(x, y, *s++, inv);
}

/* 2x 放大：列字节 b 的每个源像素展开成 2x2。 */
void RTLCD_Char2x(uint8_t x, uint8_t y, char ch, uint8_t inv)
{
    const uint8_t *g;
    uint8_t c, b;
    if ((uint8_t)ch < 0x20 || (uint8_t)ch > 0x7E) ch = '?';
    g = GLYPH((uint8_t)ch);
    for (c = 0; c < 6; c++)
    {
        uint8_t col = g[c];
        if (inv) col = (uint8_t)~col;
        for (b = 0; b < 8; b++)
        {
            uint8_t on = (uint8_t)((col >> (7 - b)) & 1);
            RTLCD_FillRect(x + (uint8_t)(c * 2), y + (uint8_t)(b * 2),
                           x + (uint8_t)(c * 2 + 1), y + (uint8_t)(b * 2 + 1), on);
        }
    }
    /* 宽 12、高 16 */
}

void RTLCD_String2x(uint8_t x, uint8_t y, const char *s, uint8_t inv)
{
    while (*s)
    {
        RTLCD_Char2x(x, y, *s++, inv);
        x += 12;
    }
}
