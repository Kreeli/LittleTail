/*
 * CH32V30x USBHS device controller port for CherryUSB.
 * Register layout follows WCH ch32v30x.h (USBHSD_TypeDef) exactly.
 */
#include "usbd_core.h"
#include "usb_usbhs_reg.h"

#ifndef CONFIG_USBDEV_EP_NUM
#define CONFIG_USBDEV_EP_NUM 8
#endif

struct ch32_usbhs_ep_state {
    uint16_t ep_mps;
    uint32_t xfer_len;
    uint32_t actual_xfer_len;
};

struct ch32_usbhs_udc {
    uint8_t dev_addr;
    __attribute__((aligned(4))) struct usb_setup_packet setup;
    struct ch32_usbhs_ep_state ep_in[CONFIG_USBDEV_EP_NUM];
    struct ch32_usbhs_ep_state ep_out[CONFIG_USBDEV_EP_NUM];
} g_ch32_usbhs_udc[CONFIG_USBDEV_MAX_BUS];

__WEAK void usb_dc_low_level_init(uint8_t busid)
{
    (void)busid;
}

__WEAK void usb_dc_low_level_deinit(uint8_t busid)
{
    (void)busid;
}

int usb_dc_init(uint8_t busid)
{
    usb_dc_low_level_init(busid);

    /* WCH sequence: clear SIE, release reset, un-suspend PHY, then enable device */
    USBHSD->CONTROL = USBHS_UC_CLR_ALL | USBHS_UC_RESET_SIE;
    USBHSD->HOST_CTRL = 0;
    Delay_Us(10);
    USBHSD->CONTROL = 0;

    USBHSD->HOST_CTRL = USBHS_UH_PHY_SUSPENDM;
    USBHSD->CONTROL = USBHS_UC_DMA_EN | USBHS_UC_INT_BUSY | USBHS_UC_SPEED_HIGH;

    USBHSD->DEV_AD = 0;
    USBHSD->INT_EN = USBHS_UIE_SETUP_ACT | USBHS_UIE_TRANSFER |
                     USBHS_UIE_DETECT | USBHS_UIE_SUSPEND;
    USBHSD->INT_FG = 0xFF;

    USBHSD->ENDP_CONFIG = 0;
    USBHSD->UEP0_DMA = (uint32_t)&g_ch32_usbhs_udc[busid].setup;
    USBHSD->UEP0_MAX_LEN = 64;
    ENDP_TX_LEN(0) = 0;
    ENDP_TX_CTRL(0) = USBHS_UEP_T_TOG_DATA1 | USBHS_UEP_T_RES_NAK;
    ENDP_RX_CTRL(0) = USBHS_UEP_R_TOG_DATA1 | USBHS_UEP_R_RES_ACK;

    /* Device pull-up - this is what the host sees on D+ */
    USBHSD->CONTROL |= USBHS_UC_DEV_PU_EN;
    return 0;
}

int usb_dc_deinit(uint8_t busid)
{
    USBHSD->CONTROL = USBHS_UC_CLR_ALL | USBHS_UC_RESET_SIE;
    USBHSD->CONTROL = 0;
    USBHSD->HOST_CTRL = 0;
    usb_dc_low_level_deinit(busid);
    return 0;
}

int usbd_set_address(uint8_t busid, const uint8_t addr)
{
    g_ch32_usbhs_udc[busid].dev_addr = addr;
    return 0;
}

int usbd_set_remote_wakeup(uint8_t busid)
{
    USBHSD->HOST_CTRL |= USBHS_UH_REMOTE_WKUP;
    return 0;
}

uint8_t usbd_get_port_speed(uint8_t busid)
{
    (void)busid;
    if ((USBHSD->SPEED_TYPE & USBHS_USB_SPEED_TYPE) == USBHS_USB_SPEED_HIGH) {
        return USB_SPEED_HIGH;
    } else if ((USBHSD->SPEED_TYPE & USBHS_USB_SPEED_TYPE) == USBHS_USB_SPEED_LOW) {
        return USB_SPEED_LOW;
    }
    return USB_SPEED_FULL;
}

int usbd_ep_open(uint8_t busid, const struct usb_endpoint_descriptor *ep)
{
    uint8_t epid = USB_EP_GET_IDX(ep->bEndpointAddress);
    uint8_t ep_type = USB_GET_ENDPOINT_TYPE(ep->bmAttributes);
    uint16_t ep_mps = USB_GET_MAXPACKETSIZE(ep->wMaxPacketSize);

    if (epid >= CONFIG_USBDEV_EP_NUM) {
        return -1;
    }

    if (USB_EP_DIR_IS_IN(ep->bEndpointAddress)) {
        g_ch32_usbhs_udc[busid].ep_in[epid].ep_mps = ep_mps;
        USBHSD->ENDP_CONFIG |= ENDP_T_EN_BIT(epid);
        ENDP_MAX_LEN(epid) = ep_mps;
        ENDP_TX_CTRL(epid) = USBHS_UEP_T_TOG_AUTO | USBHS_UEP_T_RES_NAK;
    } else {
        g_ch32_usbhs_udc[busid].ep_out[epid].ep_mps = ep_mps;
        USBHSD->ENDP_CONFIG |= ENDP_R_EN_BIT(epid);
        ENDP_MAX_LEN(epid) = ep_mps;
        ENDP_RX_CTRL(epid) = USBHS_UEP_R_TOG_AUTO | USBHS_UEP_R_RES_NAK;
    }

    if (ep_type == USB_ENDPOINT_TYPE_ISOCHRONOUS) {
        /* ISO bit lives in ENDP_TYPE; CDC does not use it */
    }
    return 0;
}

int usbd_ep_close(uint8_t busid, const uint8_t ep)
{
    uint8_t epid = USB_EP_GET_IDX(ep);
    (void)busid;
    if (USB_EP_DIR_IS_IN(ep)) {
        USBHSD->ENDP_CONFIG &= ~ENDP_T_EN_BIT(epid);
    } else {
        USBHSD->ENDP_CONFIG &= ~ENDP_R_EN_BIT(epid);
    }
    return 0;
}

int usbd_ep_set_stall(uint8_t busid, const uint8_t ep)
{
    uint8_t ep_idx = USB_EP_GET_IDX(ep);
    (void)busid;
    if (USB_EP_DIR_IS_OUT(ep)) {
        ENDP_RX_CTRL(ep_idx) = (ENDP_RX_CTRL(ep_idx) & ~USBHS_UEP_R_RES_MASK) | USBHS_UEP_R_RES_STALL;
    } else {
        ENDP_TX_CTRL(ep_idx) = (ENDP_TX_CTRL(ep_idx) & ~USBHS_UEP_T_RES_MASK) | USBHS_UEP_T_RES_STALL;
    }
    return 0;
}

int usbd_ep_clear_stall(uint8_t busid, const uint8_t ep)
{
    uint8_t ep_idx = USB_EP_GET_IDX(ep);
    (void)busid;
    if (USB_EP_DIR_IS_OUT(ep)) {
        ENDP_RX_CTRL(ep_idx) = USBHS_UEP_R_TOG_AUTO | USBHS_UEP_R_TOG_DATA0 | USBHS_UEP_R_RES_ACK;
    } else {
        ENDP_TX_CTRL(ep_idx) = USBHS_UEP_T_TOG_AUTO | USBHS_UEP_T_TOG_DATA0 | USBHS_UEP_T_RES_NAK;
    }
    return 0;
}

int usbd_ep_is_stalled(uint8_t busid, const uint8_t ep, uint8_t *stalled)
{
    (void)busid;
    if (USB_EP_DIR_IS_OUT(ep)) {
        *stalled = (ENDP_RX_CTRL(USB_EP_GET_IDX(ep)) & USBHS_UEP_R_RES_MASK) == USBHS_UEP_R_RES_STALL;
    } else {
        *stalled = (ENDP_TX_CTRL(USB_EP_GET_IDX(ep)) & USBHS_UEP_T_RES_MASK) == USBHS_UEP_T_RES_STALL;
    }
    return 0;
}

int usbd_ep_start_write(uint8_t busid, const uint8_t ep, const uint8_t *data, uint32_t data_len)
{
    uint8_t ep_idx = USB_EP_GET_IDX(ep);

    if (!data && data_len) {
        return -1;
    }
    if (data && ((uint32_t)data & 0x03)) {
        return -3;
    }
    if ((ENDP_TX_CTRL(ep_idx) & USBHS_UEP_T_RES_MASK) != USBHS_UEP_T_RES_NAK) {
        return -4;
    }

    g_ch32_usbhs_udc[busid].ep_in[ep_idx].xfer_len = data_len;
    g_ch32_usbhs_udc[busid].ep_in[ep_idx].actual_xfer_len = 0;

    if (ep_idx == 0) {
        USBHSD->UEP0_DMA = (uint32_t)data;
    } else {
        ENDP_TX_DMA(ep_idx) = (uint32_t)data;
    }

    uint16_t mps = g_ch32_usbhs_udc[busid].ep_in[ep_idx].ep_mps;
    ENDP_TX_LEN(ep_idx) = (uint16_t)MIN(data_len, mps ? mps : data_len);
    ENDP_TX_CTRL(ep_idx) = (ENDP_TX_CTRL(ep_idx) & ~USBHS_UEP_T_RES_MASK) | USBHS_UEP_T_RES_ACK;
    return 0;
}

int usbd_ep_start_read(uint8_t busid, const uint8_t ep, uint8_t *data, uint32_t data_len)
{
    uint8_t ep_idx = USB_EP_GET_IDX(ep);

    if (!data && data_len) {
        return -1;
    }
    if (data && ((uint32_t)data & 0x03)) {
        return -3;
    }

    g_ch32_usbhs_udc[busid].ep_out[ep_idx].xfer_len = data_len;
    g_ch32_usbhs_udc[busid].ep_out[ep_idx].actual_xfer_len = 0;

    if (ep_idx == 0) {
        USBHSD->UEP0_DMA = (uint32_t)data;
    } else {
        ENDP_RX_DMA(ep_idx) = (uint32_t)data;
    }

    /* Hardware accepts up to UEPn_MAX_LEN; received length is in USBHSD->RX_LEN */
    ENDP_RX_CTRL(ep_idx) = (ENDP_RX_CTRL(ep_idx) & ~USBHS_UEP_R_RES_MASK) | USBHS_UEP_R_RES_ACK;
    return 0;
}

static void handle_ep0_in(uint8_t busid)
{
    if (g_ch32_usbhs_udc[busid].setup.bmRequestType & 0x80) {
        /* toggle DATA0/DATA1 for multi-packet EP0 IN */
        ENDP_TX_CTRL(0) ^= USBHS_UEP_T_TOG_DATA1;
        ENDP_TX_CTRL(0) = (ENDP_TX_CTRL(0) & ~USBHS_UEP_T_RES_MASK) | USBHS_UEP_T_RES_NAK;

        if (g_ch32_usbhs_udc[busid].ep_in[0].xfer_len > g_ch32_usbhs_udc[busid].ep_in[0].ep_mps) {
            g_ch32_usbhs_udc[busid].ep_in[0].xfer_len -= g_ch32_usbhs_udc[busid].ep_in[0].ep_mps;
            g_ch32_usbhs_udc[busid].ep_in[0].actual_xfer_len += g_ch32_usbhs_udc[busid].ep_in[0].ep_mps;
            usbd_event_ep_in_complete_handler(busid, 0x80, g_ch32_usbhs_udc[busid].ep_in[0].actual_xfer_len);
        } else {
            g_ch32_usbhs_udc[busid].ep_in[0].actual_xfer_len += g_ch32_usbhs_udc[busid].ep_in[0].xfer_len;
            g_ch32_usbhs_udc[busid].ep_in[0].xfer_len = 0;
            usbd_event_ep_in_complete_handler(busid, 0x80, g_ch32_usbhs_udc[busid].ep_in[0].actual_xfer_len);
        }
    } else {
        USBHSD->UEP0_DMA = (uint32_t)&g_ch32_usbhs_udc[busid].setup;
        ENDP_TX_CTRL(0) = USBHS_UEP_T_TOG_DATA1 | USBHS_UEP_T_RES_NAK;
        ENDP_RX_CTRL(0) = USBHS_UEP_R_TOG_DATA1 | USBHS_UEP_R_RES_ACK;
    }
}

static void handle_non_ep0_in(uint8_t busid, uint8_t epid)
{
    /* Only change RES - never clobber DATA toggle (TOG_AUTO or manual). */
    ENDP_TX_CTRL(epid) = (ENDP_TX_CTRL(epid) & ~USBHS_UEP_T_RES_MASK) | USBHS_UEP_T_RES_NAK;

    if (g_ch32_usbhs_udc[busid].ep_in[epid].xfer_len > g_ch32_usbhs_udc[busid].ep_in[epid].ep_mps) {
        g_ch32_usbhs_udc[busid].ep_in[epid].xfer_len -= g_ch32_usbhs_udc[busid].ep_in[epid].ep_mps;
        g_ch32_usbhs_udc[busid].ep_in[epid].actual_xfer_len += g_ch32_usbhs_udc[busid].ep_in[epid].ep_mps;

        uint32_t write_count = MIN(g_ch32_usbhs_udc[busid].ep_in[epid].xfer_len,
                                   g_ch32_usbhs_udc[busid].ep_in[epid].ep_mps);
        ENDP_TX_LEN(epid) = (uint16_t)write_count;
        ENDP_TX_DMA(epid) += g_ch32_usbhs_udc[busid].ep_in[epid].ep_mps;
        ENDP_TX_CTRL(epid) = (ENDP_TX_CTRL(epid) & ~USBHS_UEP_T_RES_MASK) | USBHS_UEP_T_RES_ACK;
    } else {
        g_ch32_usbhs_udc[busid].ep_in[epid].actual_xfer_len += g_ch32_usbhs_udc[busid].ep_in[epid].xfer_len;
        g_ch32_usbhs_udc[busid].ep_in[epid].xfer_len = 0;
        usbd_event_ep_in_complete_handler(busid, 0x80 | epid, g_ch32_usbhs_udc[busid].ep_in[epid].actual_xfer_len);
    }
}

static void handle_ep0_out(uint8_t busid)
{
    uint32_t read_count = USBHSD->RX_LEN;
    read_count = MIN(read_count, g_ch32_usbhs_udc[busid].ep_out[0].xfer_len);
    g_ch32_usbhs_udc[busid].ep_out[0].actual_xfer_len += read_count;
    g_ch32_usbhs_udc[busid].ep_out[0].xfer_len -= read_count;
    usbd_event_ep_out_complete_handler(busid, 0x00, g_ch32_usbhs_udc[busid].ep_out[0].actual_xfer_len);

    if (read_count == 0) {
        USBHSD->UEP0_DMA = (uint32_t)&g_ch32_usbhs_udc[busid].setup;
        ENDP_RX_CTRL(0) = USBHS_UEP_R_TOG_DATA1 | USBHS_UEP_R_RES_ACK;
    }
}

static void handle_non_ep0_out(uint8_t busid, uint8_t epid)
{
    uint32_t read_count = USBHSD->RX_LEN;
    read_count = MIN(read_count, g_ch32_usbhs_udc[busid].ep_out[epid].xfer_len);

    /* Park RX in NAK before user callback to avoid re-entry / buffer overwrite */
    ENDP_RX_CTRL(epid) = (ENDP_RX_CTRL(epid) & ~USBHS_UEP_R_RES_MASK) | USBHS_UEP_R_RES_NAK;

    g_ch32_usbhs_udc[busid].ep_out[epid].actual_xfer_len += read_count;
    g_ch32_usbhs_udc[busid].ep_out[epid].xfer_len -= read_count;

    if ((read_count < g_ch32_usbhs_udc[busid].ep_out[epid].ep_mps) ||
        (g_ch32_usbhs_udc[busid].ep_out[epid].xfer_len == 0)) {
        usbd_event_ep_out_complete_handler(busid, epid, g_ch32_usbhs_udc[busid].ep_out[epid].actual_xfer_len);
    } else {
        ENDP_RX_DMA(epid) += g_ch32_usbhs_udc[busid].ep_out[epid].ep_mps;
        ENDP_RX_CTRL(epid) = (ENDP_RX_CTRL(epid) & ~USBHS_UEP_R_RES_MASK) | USBHS_UEP_R_RES_ACK;
    }
}

void USBD_IRQHandler(uint8_t busid)
{
    uint8_t intflag = USBHSD->INT_FG;
    uint8_t intst = USBHSD->INT_ST;

    if (intflag & USBHS_UIF_TRANSFER) {
        uint8_t token = intst & USBHS_UIS_TOKEN_MASK;
        uint8_t endp = intst & USBHS_UIS_ENDP_MASK;

        if (token == USBHS_UIS_TOKEN_IN) {
            if (endp == 0) {
                handle_ep0_in(busid);
            } else {
                handle_non_ep0_in(busid, endp);
            }

            if (g_ch32_usbhs_udc[busid].dev_addr) {
                USBHSD->DEV_AD = g_ch32_usbhs_udc[busid].dev_addr;
                g_ch32_usbhs_udc[busid].dev_addr = 0;
            }
        } else if (token == USBHS_UIS_TOKEN_OUT) {
            if (intst & USBHS_UIS_TOG_OK) {
                if (endp == 0) {
                    handle_ep0_out(busid);
                } else {
                    handle_non_ep0_out(busid, endp);
                }
            } else {
                /* toggle mismatch: re-ACK without resetting DATA toggle */
                ENDP_RX_CTRL(endp) = (ENDP_RX_CTRL(endp) & ~USBHS_UEP_R_RES_MASK) | USBHS_UEP_R_RES_ACK;
            }
        }
        USBHSD->INT_FG = USBHS_UIF_TRANSFER;
    } else if (intflag & USBHS_UIF_SETUP_ACT) {
        USBHSD->UEP0_DMA = (uint32_t)&g_ch32_usbhs_udc[busid].setup;
        ENDP_TX_CTRL(0) = USBHS_UEP_T_TOG_DATA1 | USBHS_UEP_T_RES_NAK;
        ENDP_RX_CTRL(0) = USBHS_UEP_R_TOG_DATA1 | USBHS_UEP_R_RES_NAK;
        usbd_event_ep0_setup_complete_handler(busid, (uint8_t *)&g_ch32_usbhs_udc[busid].setup);
        USBHSD->INT_FG = USBHS_UIF_SETUP_ACT;
    } else if (intflag & USBHS_UIF_DETECT) {
        /* bus reset */
        USBHSD->DEV_AD = 0;
        g_ch32_usbhs_udc[busid].dev_addr = 0;
        USBHSD->UEP0_DMA = (uint32_t)&g_ch32_usbhs_udc[busid].setup;
        ENDP_TX_CTRL(0) = USBHS_UEP_T_TOG_DATA1 | USBHS_UEP_T_RES_NAK;
        ENDP_RX_CTRL(0) = USBHS_UEP_R_TOG_DATA1 | USBHS_UEP_R_RES_ACK;
        usbd_event_reset_handler(busid);
        USBHSD->INT_FG = USBHS_UIF_DETECT;
    } else if (intflag & USBHS_UIF_SUSPEND) {
        if (USBHSD->MIS_ST & USBHS_UMS_SUSPEND) {
            usbd_event_suspend_handler(busid);
        } else {
            usbd_event_resume_handler(busid);
        }
        USBHSD->INT_FG = USBHS_UIF_SUSPEND;
    } else {
        USBHSD->INT_FG = intflag;
    }
}
