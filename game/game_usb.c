//============================================================================//
//  game_usb.c - USB CDC 通信实现
//============================================================================//
#include <stdint.h>
#include "msp430.h"
#include <USB_API/USB_Common/device.h>
#include <USB_API/USB_Common/types.h>
#include <USB_API/USB_Common/defMSP430USB.h>
#include <USB_API/USB_Common/usb.h>
#include <USB_API/USB_CDC_API/UsbCdc.h>
#include "descriptors.h"      /* CDC0_INTFNUM：本工程复合设备里 CDC 的接口号是 1 */

#include "HAL_UCS.h"          /* Init_FLL_Settle / XT2_Start */
#include <USB_API/USB_MSC_API/UsbMsc.h>
#include <USB_API/USB_MSC_API/UsbMscScsi.h>
#include <USB_API/USB_Common/UsbIsr.h>   /* SetupPacketInterruptHandler：软件兜底时复用 */
#include "game_rtlcd.h"   /* RTLCD_RestoreSpi：LCD 与 SD 共用 SPI */
#include "game_usb.h"

/* 端点描述符数组由 usb.c 定义（__no_init，地址由链接脚本固定），
   但 usb.h 没有对外声明 —— 这里补上，用于屏蔽未使用端点的中断。 */
extern tEDB  tInputEndPointDescriptorBlock[7];
extern tEDB0 tEndPoint0DescriptorBlock;   /* EP0 描述符（usb.h 未导出） */
extern tEDB tOutputEndPointDescriptorBlock[7];

/* ⚠ CDC 接口号不能硬编码 0：descriptors.h 里 CDC0_INTFNUM = 1
   （复合设备顺序是 MSC=0、HID 占 1、CDC 占 2 的数据接口），
   传错接口号会命中别的端点 / 返回错误，表现为"发送永远 busy"。 */
#define CDC_INTF      CDC0_INTFNUM
#define CDC_EP_SIZE   64u          /* USB 全速批量端点最大包长 */

static uint8_t inited = 0;
static uint8_t xt2ok  = 0;
/* ⚠ 重要：一旦主机成功枚举过（ST_ENUM_ACTIVE），就绝不再做任何"软件插拔"或
   重新初始化。这两个动作都会让 Windows 看到设备断开/重新枚举，正在打开串口的
   上位机会立刻报"打开串口失败 / 端口被占用"，而且重新枚举后 COM 号还可能变。
   自愈只对"从来没枚举成功过"的初始阶段有意义。 */
static uint8_t everEnumerated = 0;
static UsbState state = US_NONE;

/* ---- MSC 初始化 ------------------------------------------------
   完整复刻 LAB4：调用 MassStorageCommon.c 里的 msc_Init()，它会
     · disk_initialize(0)  初始化 SD 卡（无卡时自动返回失败，不影响后续）
     · USBMSC_fetchInfoStruct() 取得 API 内部结构
     · 报告"无介质"、注册 512 B 数据缓冲
   ⚠ 不调用 msc_Loop()：它内部有 __bis_SR_register(LPM0_bits + GIE)，
   在 60 fps 主循环里调用会睡到没有 USB 中断可唤醒（枚举失败时即卡死）。
   本工程不需要真实的 U 盘 SCSI 响应，只要 MSC 内部状态有效即可，
   这样 EP1 中断里访问 RWbuf_info 不会解引用空指针。
   注意本工程的 MassStorageCommon.c 曾是被 exclude 的旧版本（4 参数
   USBMSC_registerBufInfo，与当前 3 参数的 API 不匹配），已同步为 LAB4 版本。 */
#include "MassStorageCommon.h"

static uint8_t  retries = 0;          /* 自动重连次数 */

uint8_t Usb_Init(void)
{
    if (inited) return 1;                    /* 幂等 */

    /* =====================================================================
       完全照搬 LAB4 的初始化序列（ClockUSB + USB_init + connect）。
       之前自己写的版本少了 ClockUSB 里的 XT2_Start(XT2DRIVE_0)，
       而且顺序与 LAB4 不同。既然 LAB4 在同一板/同一线/同一口上稳定，
       就先原样复制，再谈优化。
       ===================================================================== */

    /* ---- 0) 上电/插线后先等 100 ms ----
       LAB4 在 USB_init() 之前执行了 disk_initialize(0)（SD 卡初始化，几十 ms）。
       本工程没有 SD 卡，但那段延时可能让 VBUS 与内部 LDO 的电容充分稳定。
       实测 A:0A48（刚写完正常）→ P:0048（之后 VUSBEN 被硬件清掉），
       是典型的 LDO 过流/欠压保护；先补上这段等待做对照。 */
    __delay_cycles(25000UL * 100UL);

    /* ---- 1) ClockUSB()：LAB4 原样 ---- */
    P5SEL |= 0x0C;                                          /* XT2 引脚 */
    UCSCTL3 = (UCSCTL3 & ~(SELREF_7)) | (SELREF__REFOCLK);   /* FLL 参考 → REFO */
    UCSCTL4 = (UCSCTL4 & ~(SELA_7))   | (SELA__REFOCLK);     /* ACLK → REFO */
    SFRIE1 &= ~OFIE;
    Init_FLL_Settle(25000, 762);                             /* MCLK 重锁 25 MHz */
    XT2_Start(XT2DRIVE_0);                                   /* 启动 XT2（USB PLL 参考） */
    SFRIE1 |= OFIE;

    /* ClockUSB 里已用死等版 XT2_Start 启动 XT2；到这里仍未返回即说明已起振 */
    xt2ok = 1;

    /* ---- 2) USB_init + 事件 ---- */
    USB_init();

    /* ⚠ USBPHYCTL 的 PUOPE（bit5, 0x0020）—— USB 端口输出使能。
       USB_init() 里只写了 USBPHYCTL = PUSEL（0x0080），这会**清掉 PUOPE**。
       TI 的参考代码就是这么写的，但本工程实测：LDO 稳定(VUSBEN 在)、
       USBCNF 有 USB_EN+PUR_EN 的情况下，USB 模块收不到任何 SETUP
       （USBIFG 恒为 0、tSetupPacket 为 ROM/RAM 残留），说明 D+/D- 实际
       没有被驱动起来。补上 PUOPE 再试。 */
    /* ⚠ USBPHYCTL 属于 USB 模块寄存器，必须在 USBKEYPID 解锁后才能写。
       上一版直接 |= PUOPE 是无效的（处于锁定态）。 */
    USBKEYPID = 0x9628;
    USBPHYCTL = PUSEL | PUOPE;     /* 一次写全：USB 引脚功能 + 输出使能 */
    USBKEYPID = 0x9600;
    USB_setEnabledEvents(kUSB_allUsbEvents);
    inited = 1;

    /* ---- 3) 若上电时线已插好，手动连接（LAB4 同） ---- */
    if (USB_connectionInfo() & kUSB_vbusPresent)
    {
        if (USB_enable() == kUSB_succeed)
        {
            /* LAB4 的顺序是 USB_init() → msc_Init() → enable/reset/connect，
               MSC 状态要在 reset 配置端点之前就绪。 */
            msc_Init();
            /* ⚠ msc_Init() 里的 disk_initialize() 会把 UCB1 改成 SD 卡的 397 kHz，
               而 UCB1 是 LCD 和 SD 卡共用的 SPI 总线 —— 不恢复的话推屏一帧要 17 ms。 */
            RTLCD_RestoreSpi();
            USB_reset();

            /* ⚠ USB_reset() 把 EP0 配成 EPCNF_STALL（"暂不接受传输"），
               而 TI 库全程没有任何地方清除 EP0 的 STALL 位 ——
               &= ~EPCNF_STALL 只用于 EP1~EP7。
               EP0 处于 STALL 时，主机的 SETUP 会被端点拒绝、不会写入
               tSetupPacket，枚举第一步就失败（实测 USBIFG 出现 RSTRIFG
               说明 RESET 到了，但 tSetupPacket 始终是 RAM 残留值）。
               这里在连接前清掉 EP0 的 STALL，让控制端点可以接收 SETUP。 */
            tEndPoint0DescriptorBlock.bIEPCNFG &= (BYTE)~EPCNF_STALL;
            tEndPoint0DescriptorBlock.bOEPCNFG &= (BYTE)~EPCNF_STALL;

            USB_connect();
            state = US_CONNECTED;
            /* D+ 上拉后给主机时间发第一个 SETUP；中断仍开着。
               LDO 稳定后仍收不到 SETUP，把等待从 50 ms 延长到 200 ms，
               排除主机枚举启动较慢的可能。 */
            __delay_cycles(25000UL * 200UL);
        }
        else
        {
            state = US_ENABLE_ERR;
        }
    }
    else
    {
        state = US_NO_VBUS;
    }

    retries = 0;          /* 本次初始化成功走完，重置自愈计数 */
    everEnumerated = 0;   /* 新一轮初始化：允许初始阶段的软件插拔自愈 */
    return 1;
}

uint8_t Usb_Xt2Ready(void) { return xt2ok; }
uint8_t Usb_IsInited(void)  { return inited; }

/* 软件兜底处理的次数：0 表示中断路径一直是好的 */
static uint16_t softRstCount = 0;



/* MassStorageCommon.c 里的共享结构（没有 SD 卡时也要能把它标记成"无介质"） */
extern USBMSC_RWbuf_Info *RWbuf_info;

/* 让 MSC 接口"会说话"。
   为什么需要：描述符里带着 U 盘（MSC）接口，但游戏从不处理 SCSI 请求，
   于是 Windows 插入后会一直探测这个"磁盘"（重试到超时，约 20~25 s）。
   这段时间里复合设备处于"正在启动"状态，CDC 的 COM 口虽然已经出现，
   但打开会失败（拒绝访问/被占用）—— 实测就是"等一会才能开串口"。
   这里每帧非阻塞地跑一次 SCSI 状态机；没有介质时直接以
   "介质不存在"结束缓冲操作，Windows 立刻就能判定"无磁盘"并收手。 */
void Usb_MscPoll(void)
{
    BYTE r;

    if (!inited) return;
    if (!RWbuf_info) return;                 /* msc_Init() 还没跑过 */
    if (USB_connectionState() != ST_ENUM_ACTIVE) return;

    r = USBMSC_poll();
    if (r == kUSBMSC_processBuffer)
    {
        if (RWbuf_info) RWbuf_info->returnCode = kUSBMSC_RWMedNotPresent;
        (void)USBMSC_bufferProcessed();
    }
}

UsbState Usb_GetState(void) { return state; }

const char *Usb_StateStr(void)
{
    switch (state)
    {
    case US_NO_XT2:        return "USB:no XT2";
    case US_NO_VBUS:       return "USB:no VBUS";
    case US_ENABLE_ERR:    return "USB:PLL err";
    case US_CONNECTED:     return "USB:connecting";
    case US_ENUM_OK:       return "USB:ready";
    case US_ENUM_SUSPENDED:return "USB:suspend";
    default:               return "USB:---";
    }
}

uint8_t Usb_IsReady(void)
{
    if (!inited) return 0;
    return (uint8_t)(USB_connectionState() == ST_ENUM_ACTIVE);
}

void Usb_Poll(void)
{
    BYTE cs;
    static uint16_t ldoTicks = 0;

    if (!inited) return;

    /* USB LDO 掉电检测与自愈：
       VUSBEN(0x0800) 只能由软件置位，实测出现过它莫名消失的情况
       （USBPWRCTL 从 0x0E48 变成 0x0048，此时 USB 完全不通、
        tSetupPacket 变成 RAM 残留值）。一旦发现就重新初始化一次，
       否则 inited 会一直为 1、幂等保护让后续所有打开尝试都变成空操作。 */
    if (!(USBPWRCTL & VUSBEN))
    {
        /* 已经枚举成功过就不再自愈：Usb_Init() 里有 reset/connect，
           会把正在使用的串口打断。这种情况让用户拔插一次线更可靠。 */
        if (everEnumerated) { state = US_NO_VBUS; return; }

        if (++ldoTicks > 60u)              /* 约 1 s，避免抖动时反复重init */
        {
            ldoTicks = 0;
            /* Usb_Init() 内部有 100 ms + 200 ms 延时，在游戏主循环里调用会
               卡掉 18 帧。所以自愈最多做 3 次就放弃（枚举成功会清零 retries）。 */
            if (retries < 3u)
            {
                retries++;
                inited = 0;                /* 允许 Usb_Init() 重跑 */
                Usb_Init();
            }
        }
        state = US_NO_VBUS;                /* 期间对外报"不可用" */
        return;
    }
    ldoTicks = 0;

    /* ===== 软件事件兜底 =====
       正常情况下 RESET/SETUP 由 iUsbInterruptHandler() 分发。实测出现过
       USBIE = 0x0020（只剩 RESRIE）——那是 USB_suspend() 留下的状态，
       此时 RSTRIE/SETUPIE 都是关的：主机发 RESET 只会把 RSTRIFG 置位，
       中断不产生，标志永远挂着，设备再也走不到 SETUP 阶段。
       这里直接轮询标志，把中断没接住的 RESET 补上。 */
    /* 只在中断确实接不住（对应 IE 关闭）时才由软件接管并清标志，
       否则会把本该交给 ISR 的事件抢掉。 */
    if ((USBIFG & RSTRIFG) && !(USBIE & RSTRIE))
    {
        softRstCount++;
        USBIFG &= (WORD)~RSTRIFG;          /* 写 0 清除 */
        /* 先拉低 D+ 再拉起才是真正的"软件插拔"：此时 PUR_EN 本来就是 1，
           只调 USB_connect() 不会产生新的上升沿，主机不会重新识别。
           但只在"本次开机还没枚举成功过"时才这么做。
           枚举成功之后再拉低 D+ 会让 Windows 丢掉这个设备实例，
           上位机正在用的 COM 口立刻失效。 */
        if (!everEnumerated && softRstCount <= 3u)
        {
            USB_disconnect();
            __delay_cycles(25000UL * 20UL);/* 20 ms，够主机检测到拔出 */
        }
        USB_reset();
        tEndPoint0DescriptorBlock.bIEPCNFG &= (BYTE)~EPCNF_STALL;
        tEndPoint0DescriptorBlock.bOEPCNFG &= (BYTE)~EPCNF_STALL;
        USBIE = SETUPIE | RSTRIE | SUSRIE;     /* 0x00C4，重新武装 */
        USB_connect();
    }

    /* SETUP 同理：若 USBIE 里没有 SETUPIE，标志会一直挂着而没人处理。
       这里直接调用栈自己的 SetupPacketInterruptHandler() 把包解析掉，
       否则主机的第一个 GET_DESCRIPTOR 会被静默丢弃，枚举永远起不来。
       兜底处理的次数不影响判定，只用于自愈。 */
    if ((USBIFG & SETUPIFG) && !(USBIE & SETUPIE))
    {
        uint16_t sr = __get_SR_register();

        __disable_interrupt();             /* 与端点中断互斥，避免改到同一份 EP0 状态 */
        (void)SetupPacketInterruptHandler();   /* 真的去解析这个 SETUP，而不是丢包 */
        USBIFG &= (WORD)~SETUPIFG;
        USBIE |= SETUPIE;                  /* 打开，让后续 SETUP 走正常中断 */
        if (sr & GIE) __enable_interrupt();   /* 只恢复 GIE（CCS 无 __set_SR_register） */
    }

    /* 把 USB 栈的连接状态映射成可显示的状态。
       注意：USB_connectionState() 只是读寄存器，不会阻塞。 */
    cs = USB_connectionState();
    switch (cs)
    {
    case ST_ENUM_ACTIVE:
        state = US_ENUM_OK; retries = 0; everEnumerated = 1;
        break;
    case ST_ENUM_IN_PROGRESS: state = US_CONNECTED;      break;
    case ST_ENUM_SUSPENDED:
    case ST_NOENUM_SUSPENDED: state = US_ENUM_SUSPENDED; break;
    case ST_USB_DISCONNECTED:
        if (state == US_ENUM_OK) state = US_NO_VBUS;     /* 线被拔掉 */
        break;
    default: break;
    }

    /* 推进 MSC(SCSI) 状态机：不这么做 Windows 会一直探测那个"U 盘"，
       复合设备迟迟不进入"就绪"，COM 口能看见但打不开。 */
    Usb_MscPoll();

    /* ⚠ 这里原本每 3 秒做一次 USB_reset() + USB_connect()（软件模拟插拔）。
       但 M_USBWAIT 的等待窗口是 10 秒 —— 期间会撞上 3 次 reset，
       而每次 USB_reset() 都会把 bEnumerationStatus 清零、重配端点、
       重新拉 D+，正好打断主机进行中的枚举（典型 1~2 秒）。
       这是一个自己制造的竞争，已移除：M_USBWAIT 期间 CPU 本就空闲，
       主机有充足时间完成枚举；真遇到"主机错过上升沿"，拔插一次线即可。 */
}

uint16_t Usb_Write(const uint8_t *buf, uint16_t len)
{
    uint16_t off = 0;

    if (!Usb_IsReady()) return 0;

    while (off < len)
    {
        uint16_t chunk = (uint16_t)(len - off);
        if (chunk > CDC_EP_SIZE) chunk = CDC_EP_SIZE;

        /* 返回 kUSB_succeed 表示已放入端点并交由 USB 栈发送；
           kUSB_busy 表示上一个包还没发完 —— 剩下的直接丢弃，
           遥测数据"宁缺勿阻塞"。 */
        /* ⚠ 返回值是 kUSBCDC_sendStarted(0x01)，不是通用 API 的 kUSB_succeed(0x00)。
           写成 != kUSB_succeed 会把每一次成功发送都判为失败并 break，
           结果就是"USB 通了但一个字都发不出来"。 */
        if (USBCDC_sendData((const BYTE *)(buf + off), chunk, CDC_INTF) != kUSBCDC_sendStarted)
            break;

        off += chunk;
    }
    return off;
}

uint16_t Usb_Read(uint8_t *buf, uint16_t maxLen)
{
    BYTE    n;
    uint16_t got = 0;

    if (!inited || maxLen == 0) return 0;

    n = USBCDC_bytesInUSBBuffer(CDC_INTF);
    if (n == 0) return 0;
    if (n > maxLen) n = (BYTE)maxLen;

    /* ⚠ 与 sendData 同类问题：成功返回 kUSBCDC_receiveCompleted(0x05)，
       而通用 API 的 kUSB_succeed 是 0x00。写成后者会让读永远返回 0。 */
    if (USBCDC_receiveData(buf, n, CDC_INTF) == kUSBCDC_receiveCompleted)
        got = n;
    return got;
}
