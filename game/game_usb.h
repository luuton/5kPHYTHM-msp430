//============================================================================//
//  game_usb.h - USB CDC 通信（USCI 之外的独立外设，PU.0/PU.1）
//
//  为什么用 USB CDC 替代 UART：
//    · 一根线同时供电和通信，不需要外接 USB-TTL
//    · CDC 发送是非阻塞的（忙则丢弃），而 UART 一行 69 字符要阻塞 6 ms
//    · 电脑端免驱，出现一个 COM 口，现有五行 ASCII 遥测协议可原样复用
//
//  时钟：USB 需要 4 MHz XT2 作 PLL 参考，序列完整复刻 LAB4 的 ClockUSB()
//    （P5SEL 切引脚 → FLL 以 REFO 锁 25 MHz → XT2_Start），顺序不能乱。
//    ⚠ LCD 与 SD 卡共用 USCI_B1：msc_Init() 会把 UCB1 改成 397 kHz，
//    推屏一帧从 2 ms 变成 17 ms，所以之后必须调 RTLCD_RestoreSpi()。
//
//  引脚：PU.0/PU.1（USB D+/D-），与触摸/液晶/UART/加速度计均不冲突。
//============================================================================//
#ifndef GAME_USB_H
#define GAME_USB_H

#include <stdint.h>

/* USB 启动状态（用于开机屏显示，便于定位问题） */
typedef enum {
    US_NONE = 0,        /* 未初始化 */
    US_NO_XT2,          /* XT2 未起振 —— USB PLL 无参考，物理上不可用 */
    US_NO_VBUS,         /* 没有检测到 VBUS —— USB 线没插到板子的设备口 */
    US_ENABLE_ERR,      /* USB_enable 失败（PLL 未锁定/带隙未就绪） */
    US_CONNECTED,       /* 已连接，等待主机枚举 */
    US_ENUM_OK,         /* 枚举完成，可以通信 */
    US_ENUM_SUSPENDED   /* 主机挂起 */
} UsbState;

/* 非阻塞地推进 MSC(SCSI) 状态机，让主机尽快判定"无磁盘" */
extern void Usb_MscPoll(void);
/* USB 是否已初始化（设置界面里显示 ON/OFF 用） */
extern uint8_t Usb_IsInited(void);

/* 当前状态（主循环每帧更新） */
extern UsbState Usb_GetState(void);
/* 状态的短文本，供屏幕显示 */
extern const char *Usb_StateStr(void);

/* 初始化 USB（CDC 复合设备）。
   上电时若 USB 线已插好，不会产生 VbusOn 事件，这里会手动连接。
   返回 1 = 已初始化（不代表已枚举）。 */
extern uint8_t Usb_Init(void);

/* 主循环每帧调用：非阻塞，几微秒。用于推进 CDC 发送与接收状态。 */
extern void Usb_Poll(void);

/* XT2（USB PLL 的参考晶振）是否已成功起振并稳定。
   USB 事件处理里必须先问这个，否则 USB_enable() 内部死等版 XT2_Start()
   会把 CPU 卡死在中途。 */
extern uint8_t Usb_Xt2Ready(void);

/* CDC 是否已枚举完成（电脑已识别设备）。发送前应检查。 */
extern uint8_t Usb_IsReady(void);

/* 非阻塞发送：返回实际接受的字节数（0 = 未就绪或端点忙）。
   单次最多 64 字节（USB 全速端点大小），超出部分需要调用方分段。 */
extern uint16_t Usb_Write(const uint8_t *buf, uint16_t len);

/* 非阻塞接收：返回实际读到的字节数（0 = 无数据）。 */
extern uint16_t Usb_Read(uint8_t *buf, uint16_t maxLen);

#endif /* GAME_USB_H */
