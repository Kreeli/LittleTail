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

#endif /* USB_USBHS_REG_H */
