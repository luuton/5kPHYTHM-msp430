// (c)2009 by Texas Instruments Incorporated, All Rights Reserved.
/*----------------------------------------------------------------------------+
|                                                                             |
|                              Texas Instruments                              |
|                                                                             |
|                          MSP430 USB-Example (CDC/HID Driver)                |
|                                                                             |
+-----------------------------------------------------------------------------+
|  Source: usb_eventHandling.c, File Version 1.00 2009/12/03                  |
|  Author: RSTO                                                               |
|                                                                             |
|  Description:                                                               |
|  Event-handling placeholder functions.                                      |
|  All functios are called in interrupt context.                              |
|                                                                             |
|  WHO          WHEN         WHAT                                             |
|  ---          ----------   ------------------------------------------------ |
|  RSTO         2008/09/03   born                                             |
|  RSTO         2008/12/23   enhancements of CDC API                          |
|  MSP,Biju     2009/10/20   composite support changes                        |
|  RSTO         2009/11/06   added USB_handleEnumCompleteEvent()              |
|  Biju,MSP     2010/06/09   Added handleBufferEvent()                        |
+----------------------------------------------------------------------------*/

#include "..\USB_Common\device.h"
#include "..\USB_Common\types.h"            // Basic Type declarations
#include "descriptors.h"
#include "usb.h"                        // USB-specific Data Structures

extern BYTE *RWbuf_X, *RWbuf_Y;


/*
If this function gets executed, it's a sign that the output of the USB PLL has failed.
returns TRUE to keep CPU awake
*/
BYTE USB_handleClockEvent()
{
    //TO DO: You can place your code here

    return TRUE;
}

/*
If this function gets executed, it indicates that a valid voltage has just been applied to the VBUS pin.
returns TRUE to keep CPU awake
*/
/* 由 game/game_usb.c 提供：XT2 是否已起振并稳定 */
extern unsigned char Usb_Xt2Ready(void);

BYTE USB_handleVbusOnEvent()
{
    /* ⚠ 原厂实现直接调 USB_enable()，而 USB_enable() 内部用死等版
       XT2_Start()（while (SFRIFG1 & OFIFG)）。
       XT2 不起振时，VBUS 中断每触发一次就把 CPU 卡死在这里，
       整机失去响应 —— 本项目实测"卡在 LOGO 屏、按键无反应"即由此而来。

       因此先确认 XT2 就绪再往下走；未就绪就只跳过本次连接，
       由 game_usb 的 Usb_Init() 在开机时统一判定。 */
    if (!Usb_Xt2Ready())
        return TRUE;

    /* 再查一次晶振故障标志：USB_enable() 内部用死等版 XT2_Start()，
       若此刻 OFIFG 仍置位（XT1/XT2 有故障），调用它会卡死在这个中断里。 */
    if (SFRIFG1 & OFIFG)
        return TRUE;

    if (USB_enable() == kUSB_succeed)
    {
        USB_reset();
        USB_connect();  // generate rising edge on DP -> the host enumerates our device as full speed device
    }
    return TRUE;
}

/*
If this function gets executed, it indicates that a valid voltage has just been removed from the VBUS pin.
returns TRUE to keep CPU awake
*/
BYTE USB_handleVbusOffEvent()
{
    //TO DO: You can place your code here

    return TRUE;
}

/*
If this function gets executed, it indicates that the USB host has issued a USB reset event to the device.
returns TRUE to keep CPU awake
*/
BYTE USB_handleResetEvent()
{
    /* ⚠ 这里曾经调用 Usb_MaskUnusedEndpoints() 去 STALL MSC/HID 端点。
       但描述符已恢复为完整的 MSC+HID+CDC，主机枚举前先发 USB reset，
       重置后若把这两个端点 STALL，主机探测接口时就会失败。
       描述符与端点行为必须一致：描述符声明了接口，端点就不能 STALL。 */
    return TRUE;
}

/*
If this function gets executed, it indicates that the USB host has chosen to suspend this device after a period of active operation.
returns TRUE to keep CPU awake
*/
BYTE USB_handleSuspendEvent()
{
    //TO DO: You can place your code here

    return TRUE;
}

/*
If this function gets executed, it indicates that the USB host has chosen to resume this device after a period of suspended operation.
returns TRUE to keep CPU awake
*/
BYTE USB_handleResumeEvent()
{
    //TO DO: You can place your code here

    return TRUE;   //return TRUE to wake up after interrupt
}

/*
If this function gets executed, it indicates that the USB host has enumerated this device :
after host assigned the address to the device.
returns TRUE to keep CPU awake
*/
BYTE USB_handleEnumCompleteEvent()
{
    //TO DO: You can place your code here

    return TRUE;   //return TRUE to wake up after interrupt
}


#ifdef _CDC_

/*
This event indicates that data has been received for interface intfNum, but no data receive operation is underway.
returns TRUE to keep CPU awake
*/
BYTE USBCDC_handleDataReceived(BYTE intfNum)
{
    //TO DO: You can place your code here

    return TRUE;   //return TRUE to wake up after data was received
}

/*
This event indicates that a send operation on interface intfNum has just been completed.
returns TRUE to keep CPU awake
*/
BYTE USBCDC_handleSendCompleted(BYTE intfNum)
{
    //TO DO: You can place your code here

    return FALSE;   //return FALSE to go asleep after interrupt (in the case the CPU slept before interrupt)
}

/*
This event indicates that a receive operation on interface intfNum has just been completed.
*/
BYTE USBCDC_handleReceiveCompleted(BYTE intfNum)
{
    //TO DO: You can place your code here

    return FALSE;   //return FALSE to go asleep after interrupt (in the case the CPU slept before interrupt)
}

#endif // _CDC_

#ifdef _HID_
/*
This event indicates that data has been received for interface intfNum, but no data receive operation is underway.
returns TRUE to keep CPU awake
*/
BYTE USBHID_handleDataReceived(BYTE intfNum)
{
    //TO DO: You can place your code here

    return TRUE;   //return TRUE to wake up after data was received
}

/*
This event indicates that a send operation on interface intfNum has just been completed.
returns TRUE to keep CPU awake
*/
BYTE USBHID_handleSendCompleted(BYTE intfNum)
{
    //TO DO: You can place your code here

    return FALSE;   //return FALSE to go asleep after interrupt (in the case the CPU slept before interrupt)
}

/*
This event indicates that a receive operation on interface intfNum has just been completed.
*/
BYTE USBHID_handleReceiveCompleted(BYTE intfNum)
{
    //TO DO: You can place your code here

    return FALSE;   //return FALSE to go asleep after interrupt (in the case the CPU slept before interrupt)
}

#endif // _HID_

#ifdef _MSC_
BYTE USBMSC_handleBufferEvent(VOID)
{

    return TRUE;

}
#endif // _MSC_


/*----------------------------------------------------------------------------+
| End of source file                                                          |
+----------------------------------------------------------------------------*/
/*------------------------ Nothing Below This Line --------------------------*/
