/*
 * CH32V30x USBHS device registers - aligned with WCH ch32v30x.h / ch32v30x_usb.h
 *
 * Previous CherryUSB usbhs port used a different IP layout (CONTROL/BASE_MODE/
 * UEP_TX_EN...), which does not match CH32V305/307 silicon. That left the PHY
 * suspended (HOST_CTRL.PHY_SUSPENDM never set) so DP/DM never presented a
 * pull-up and the host never saw a device.
 */
#ifndef USB_USBHS_REG_H
#define USB_USBHS_REG_H

#include "ch32v30x.h"

/* R8_USB_CTRL */
#define USBHS_UC_HOST_MODE          0x80
#define USBHS_UC_SPEED_TYPE         0x60
#define USBHS_UC_SPEED_LOW          0x40
#define USBHS_UC_SPEED_FULL         0x00
#define USBHS_UC_SPEED_HIGH         0x20
#define USBHS_UC_DEV_PU_EN          0x10
#define USBHS_UC_INT_BUSY           0x08
#define USBHS_UC_RESET_SIE          0x04
#define USBHS_UC_CLR_ALL            0x02
#define USBHS_UC_DMA_EN             0x01

/* R8_USB_INT_EN / R8_USB_INT_FG (same bit layout) */
#define USBHS_UIE_DEV_NAK           0x80
#define USBHS_UIE_ISO_ACT           0x40
#define USBHS_UIE_SETUP_ACT         0x20
#define USBHS_UIE_FIFO_OV           0x10
#define USBHS_UIE_SOF_ACT           0x08
#define USBHS_UIE_SUSPEND           0x04
#define USBHS_UIE_TRANSFER          0x02
#define USBHS_UIE_DETECT            0x01
#define USBHS_UIE_BUS_RST           0x01

#define USBHS_UIF_ISO_ACT           0x40
#define USBHS_UIF_SETUP_ACT         0x20
#define USBHS_UIF_FIFO_OV           0x10
#define USBHS_UIF_HST_SOF           0x08
#define USBHS_UIF_SUSPEND           0x04
#define USBHS_UIF_TRANSFER          0x02
#define USBHS_UIF_DETECT            0x01
#define USBHS_UIF_BUS_RST           0x01

/*
 * UIF_ISO_ACT 是 USBHS 独有的"同步活动"标志（USBFS 的 INT_FG 里没有这一位，
 * 所以那套 USBFS 的 UAC 例程都不管它，照抄过来就会漏）。
 * 踩过的坑：把它的中断使能（USBHS_UIE_ISO_ACT）打开，USB 中断会反复重入、
 * CPU 被占满，表现是"设备在、串口打不开"。
 * 本工程的做法：**不使能**它的中断，但每次进 USB 中断都写 1 把它清掉，
 * 免得它一直挂着把同步端点的完成事件堵住。
 */
#define USBHS_UIF_TRANSFER_MASK     (USBHS_UIF_TRANSFER | USBHS_UIF_ISO_ACT)

/* R8_USB_INT_ST */
#define USBHS_UIS_IS_NAK            0x80
#define USBHS_UIS_TOG_OK            0x40
#define USBHS_UIS_TOKEN_MASK        0x30
#define USBHS_UIS_TOKEN_OUT         0x00
#define USBHS_UIS_TOKEN_SOF         0x10
#define USBHS_UIS_TOKEN_IN          0x20
#define USBHS_UIS_TOKEN_SETUP       0x30
#define USBHS_UIS_ENDP_MASK         0x0F

/* R8_USB_MIS_ST */
#define USBHS_UMS_SOF_PRES          0x80
#define USBHS_UMS_SOF_ACT           0x40
#define USBHS_UMS_SIE_FREE          0x20
#define USBHS_UMS_R_FIFO_RDY        0x10
#define USBHS_UMS_BUS_RESET         0x08
#define USBHS_UMS_SUSPEND           0x04
#define USBHS_UMS_DEV_ATTACH        0x02
#define USBHS_UMS_SPLIT_CAN         0x01

/* R8_USB_SPEED_TYPE */
#define USBHS_USB_SPEED_TYPE        0x03
#define USBHS_USB_SPEED_LOW         0x02
#define USBHS_USB_SPEED_FULL        0x00
#define USBHS_USB_SPEED_HIGH        0x01

/* R8_UHOST_CTRL / HOST_CTRL - PHY_SUSPENDM lives HERE, not in CONTROL */
#define USBHS_UH_SOF_EN             0x80
#define USBHS_UH_PHY_SUSPENDM       0x10
#define USBHS_UH_REMOTE_WKUP        0x08

/* R32_UEP_CONFIG */
#define USBHS_UEP0_T_EN             0x00000001
#define USBHS_UEP1_T_EN             0x00000002
#define USBHS_UEP2_T_EN             0x00000004
#define USBHS_UEP3_T_EN             0x00000008
#define USBHS_UEP4_T_EN             0x00000010
#define USBHS_UEP5_T_EN             0x00000020
#define USBHS_UEP6_T_EN             0x00000040
#define USBHS_UEP7_T_EN             0x00000080
#define USBHS_UEP0_R_EN             0x00010000
#define USBHS_UEP1_R_EN             0x00020000
#define USBHS_UEP2_R_EN             0x00040000
#define USBHS_UEP3_R_EN             0x00080000
#define USBHS_UEP4_R_EN             0x00100000
#define USBHS_UEP5_R_EN             0x00200000
#define USBHS_UEP6_R_EN             0x00400000
#define USBHS_UEP7_R_EN             0x00800000

/* R8_UEPn_TX_CTRL / R8_UEPn_RX_CTRL - response codes differ from old port */
#define USBHS_UEP_T_TOG_AUTO        0x20
#define USBHS_UEP_T_TOG_MASK        0x18
#define USBHS_UEP_T_TOG_DATA0       0x00
#define USBHS_UEP_T_TOG_DATA1       0x08
#define USBHS_UEP_T_TOG_DATA2       0x10
#define USBHS_UEP_T_TOG_MDATA       0x18
#define USBHS_UEP_T_RES_MASK        0x03
#define USBHS_UEP_T_RES_ACK         0x00
#define USBHS_UEP_T_RES_NYET        0x01
#define USBHS_UEP_T_RES_NAK         0x02
#define USBHS_UEP_T_RES_STALL       0x03

#define USBHS_UEP_R_TOG_AUTO        0x20
#define USBHS_UEP_R_TOG_MASK        0x18
#define USBHS_UEP_R_TOG_DATA0       0x00
#define USBHS_UEP_R_TOG_DATA1       0x08
#define USBHS_UEP_R_TOG_DATA2       0x10
#define USBHS_UEP_R_TOG_MDATA       0x18
#define USBHS_UEP_R_RES_MASK        0x03
#define USBHS_UEP_R_RES_ACK         0x00
#define USBHS_UEP_R_RES_NYET        0x01
#define USBHS_UEP_R_RES_NAK         0x02
#define USBHS_UEP_R_RES_STALL       0x03

/*
 * 收发两个方向的 TOG_AUTO 是同一个位（bit5），写代码时别被 T_/R_ 前缀绕进去。
 *
 * 同步(ISO)端点三条铁律（写错任何一条都会表现为"回调只进一次"或中断风暴）：
 *   1. 翻转位固定 DATA0，**不能**带 TOG_AUTO：同步 OUT 的 PID 由主机固定为 DATA0，
 *      自动翻转会让硬件第二次开始期待 DATA1 → TOG_OK=0 → 包被 ACK 掉但不上交；
 *   2. 响应位（RES）只表示"这一包收不收/发不发"，同步端点不做握手。
 *      本工程用 NYET 表示"已挂好"（TinyUSB 的 CH32 USBHS 端口写法）；
 *      沁恒量产固件用 ACK，两种都能跑，见 usb_dc_usbhs.c 的 CH32_USBHS_ISO_RES；
 *   3. UEPn_MAX_LEN **每个索引只有一份，收发共用**：同一索引的两个方向要不同
 *      包长时（音频数据 1024 OUT / 反馈 4 IN）必须用不同索引。
 */
#define USBHS_UEP_TOG_AUTO          0x20

/* Per-endpoint field accessors (WCH layout: TX_LEN u16 | TX_CTRL u8 | RX_CTRL u8) */
#define ENDP_TX_LEN(ep)   (*((__IO uint16_t *)&(USBHSD->UEP0_TX_LEN) + (ep) * 2))
#define ENDP_TX_CTRL(ep)  (*((__IO uint8_t *)&(USBHSD->UEP0_TX_CTRL) + (ep) * 4))
#define ENDP_RX_CTRL(ep)  (*((__IO uint8_t *)&(USBHSD->UEP0_RX_CTRL) + (ep) * 4))
#define ENDP_MAX_LEN(ep)  (*((__IO uint16_t *)&(USBHSD->UEP0_MAX_LEN) + (ep) * 2))

static inline __IO uint32_t *ENDP_TX_DMA_PTR(uint8_t ep)
{
    return (ep == 0) ? &USBHSD->UEP0_DMA : (__IO uint32_t *)&USBHSD->UEP1_TX_DMA + (ep - 1);
}
static inline __IO uint32_t *ENDP_RX_DMA_PTR(uint8_t ep)
{
    return (ep == 0) ? &USBHSD->UEP0_DMA : (__IO uint32_t *)&USBHSD->UEP1_RX_DMA + (ep - 1);
}
#define ENDP_TX_DMA(ep)      (*ENDP_TX_DMA_PTR(ep))
#define ENDP_RX_DMA(ep)      (*ENDP_RX_DMA_PTR(ep))

#define ENDP_T_EN_BIT(ep) ((uint32_t)1 << (ep))
#define ENDP_R_EN_BIT(ep) ((uint32_t)1 << ((ep) + 16))

/*
 * R32_UEP_TYPE：bit n = UEPn_T_TYPE（发送/IN），bit n+16 = UEPn_R_TYPE（接收/OUT）。
 * 置 1 表示该端点方向为同步(isochronous)传输，置 0 为控制/批量/中断。
 * 同步端点不写这一位会被硬件当成批量端点，ISO 事务无法收发。
 * （见 ch32v30x_usb.h 的 USBHS_UEPn_T_TYPE / USBHS_UEPn_R_TYPE 定义）
 */
#define ENDP_T_TYPE_BIT(ep) ((uint32_t)1 << (ep))
#define ENDP_R_TYPE_BIT(ep) ((uint32_t)1 << ((ep) + 16))

#endif /* USB_USBHS_REG_H */
