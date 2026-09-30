/*
 * CH32V30x USBHS device controller port for CherryUSB.
 * 寄存器布局与沁恒 ch32v30x.h (USBHSD_TypeDef) / ch32v30x_usb.h 完全一致。
 *
 * 本文件的数据通路写法对齐两份"在同一个 USBHS IP 上被验证过"的实现：
 *   1) TinyUSB  src/portable/wch/dcd_ch32_usbhs.c
 *      （CH32V305/307 上跑官方 uac2_headset：同步 OUT + 同步 IN + 反馈端点）
 *   2) 沁恒      EVT/EXAM/USB/USBHS/DEVICE 下各例程的 ch32v30x_usbhs_device.c
 *      （量产 384k UAC2 声卡固件）
 * 两边在批量/控制端点上写法不同，但在**同步(ISO)端点**上结论一致，而声卡踩的
 * 恰好全是同步端点的坑：
 *
 *   a) 类型位：同步端点必须把 R32_UEP_TYPE 里对应的 T_TYPE/R_TYPE 位置 1，
 *      否则硬件按批量端点处理，同步事务根本收发不了。
 *   b) DATA 翻转：同步端点固定 DATA0，**不能**用 TOG_AUTO。
 *      同步 OUT 事务主机永远发 DATA0，一旦让硬件自动翻转，硬件第二次就期待
 *      DATA1，TOG_OK=0 → 包被 ACK 掉却不上交 → 回调"只进一次"。
 *   c) 响应位：同步端点不做握手，响应码只表示"这一包收不收/发不发"。
 *      TinyUSB 用 NYET 表示"已挂好"，沁恒的量产固件用 ACK。见 CH32_USBHS_ISO_RES。
 *   d) 同步 OUT 不按 TOG_OK 过滤（c 的补充保险）；同步 IN 只改响应位、不碰翻转位。
 *   e) UEPn_MAX_LEN **每个端点索引只有一份寄存器**，IN/OUT 共用：同一个索引的
 *      两个方向要不同包长时，后打开的一方会覆盖前者，必须换索引。
 *      本工程：0x03 OUT=1024 / 0x84 IN=4，索引 3 / 4，互不影响。
 *   f) UIF_ISO_ACT 是 USBHS 独有的"同步活动"标志，**不要**使能它的中断
 *      （UIE_ISO_ACT）：使能后 USB 中断会反复重入、CPU 被占满。
 *      这里只在每次进中断时顺手写 1 清掉它。
 */
#include <string.h>

#include "usbd_core.h"
#include "usb_usbhs_reg.h"

#ifndef CONFIG_USBDEV_EP_NUM
#define CONFIG_USBDEV_EP_NUM 8
#endif

#ifndef CONFIG_USBDEV_REQUEST_BUFFER_LEN
#define CONFIG_USBDEV_REQUEST_BUFFER_LEN 512
#endif

/*
 * EP0 的 DMA 缓冲大小 = CherryUSB 一次控制传输的最大数据量。
 * 整个 EP0 生命周期里 UEP0_DMA **只指向这一个缓冲，从不改动**：
 * SETUP 包、IN 数据、OUT 数据都落在这里，再由软件搬进/搬出 core 的 req_data。
 * 这样做的原因：
 *   - 硬件收到 SETUP 是"先 DMA 再中断"，如果 DMA 指针当时正指向别处
 *     （比如上一笔数据阶段的用户缓冲），setup 包就丢了；
 *   - 0 长度状态包调用 usbd_ep_start_write(.., NULL, 0) 不能把 DMA 指针写成 0。
 */
#define CH32_USBHS_EP0_DMA_SIZE CONFIG_USBDEV_REQUEST_BUFFER_LEN

/*
 * 同步端点的"允许收发"响应码。
 * 低两位：00=ACK 01=NYET 10=NAK 11=STALL。
 * TinyUSB 的 CH32 USBHS 端口用 NYET（并注明"ISO 传输不受 INT_BUSY 约束"），
 * 沁恒 ch32v30x_usbhs_device.c 用 ACK 且只在初始化时写一次 —— 两种都能跑，
 * 说明这一位对同步端点并不严格区分 ACK/NYET。
 * 这里跟随 TinyUSB（最新、有人在维护）。如果上板发现同步 OUT 一包都收不到，
 * 把下面改成 USBHS_UEP_R_RES_ACK 再试一次即可（只影响同步端点）。
 */
#ifndef CH32_USBHS_ISO_RES
#define CH32_USBHS_ISO_RES USBHS_UEP_R_RES_NYET
#endif

/*
 * 同步完成事件走哪条路？这一位决定 INT_EN 里要不要打开 UIE_ISO_ACT。
 *
 * 默认 0（不打开）：WCH 的量产 UAC2 固件和 TinyUSB 的 CH32 USBHS 端口都是这么做的
 *   —— 同步端点的完成同样置 UIF_TRANSFER，走普通传输分支即可。
 *
 * 如果你的板子上出现"音频 OUT 回调只进一次"（USB 抓包正常、设备不掉线、串口也正常），
 * 把这里改成 1 再试：打开后 INT_EN 会加上 UIE_ISO_ACT，中断里把 ISO_ACT 单独置位
 * 也当成一次同步完成来处理。
 * 注意：ISO_ACT 必须"使能 + 每进一次中断就写 1 清掉"两个动作配套做。之前踩的坑是
 * 只使能没清（或只清没使能），前者表现为 USB 中断风暴、COM 口打不开，后者表现为
 * 回调只进一次。
 */
#ifndef CH32_USBHS_ISO_ACT_INT
#define CH32_USBHS_ISO_ACT_INT 0
#endif

struct ch32_usbhs_ep_state {
    uint16_t ep_mps;
    uint32_t xfer_len;
    uint32_t actual_xfer_len;
};

struct ch32_usbhs_udc {
    uint8_t dev_addr;
    uint8_t dev_addr_pending; /* SET_ADDRESS 要等状态阶段走完再写 DEV_AD */

    /* 本包 SETUP 的副本：数据阶段会覆盖 ep0_dma，handle_ep0_in 还要用 bmRequestType */
    struct usb_setup_packet setup;

    __attribute__((aligned(4))) uint8_t ep0_dma[CH32_USBHS_EP0_DMA_SIZE];
    /* OUT 数据阶段回填的目标地址（core 的 req_data） */
    uint8_t *ep0_out_ptr;

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

/* -------------------------------------------------------------------------
 * 端点类型 / 响应位
 * 直接读 ENDP_TYPE，任何时候都和硬件里的配置一致，不用另存一份状态。
 * ------------------------------------------------------------------------- */
static inline uint8_t ch32_usbhs_iso_in(uint8_t ep)
{
    return (uint8_t)((USBHSD->ENDP_TYPE >> ep) & 0x01u);
}

static inline uint8_t ch32_usbhs_iso_out(uint8_t ep)
{
    return (uint8_t)((USBHSD->ENDP_TYPE >> (ep + 16)) & 0x01u);
}

/* 允许发送（非同步端点只改响应位，翻转位交给 TOG_AUTO；同步端点固定 DATA0） */
static void ch32_usbhs_ep_in_arm(uint8_t ep)
{
    if (ep && ch32_usbhs_iso_in(ep)) {
        ENDP_TX_CTRL(ep) = USBHS_UEP_T_TOG_DATA0 | CH32_USBHS_ISO_RES;
    } else {
        ENDP_TX_CTRL(ep) = (ENDP_TX_CTRL(ep) & ~USBHS_UEP_T_RES_MASK) | USBHS_UEP_T_RES_ACK;
    }
}

/* 暂停发送（NAK） */
static void ch32_usbhs_ep_in_idle(uint8_t ep)
{
    if (ep && ch32_usbhs_iso_in(ep)) {
        ENDP_TX_CTRL(ep) = USBHS_UEP_T_TOG_DATA0 | USBHS_UEP_T_RES_NAK;
    } else {
        ENDP_TX_CTRL(ep) = (ENDP_TX_CTRL(ep) & ~USBHS_UEP_T_RES_MASK) | USBHS_UEP_T_RES_NAK;
    }
}

/* 允许接收 */
static void ch32_usbhs_ep_out_arm(uint8_t ep)
{
    if (ep && ch32_usbhs_iso_out(ep)) {
        ENDP_RX_CTRL(ep) = USBHS_UEP_R_TOG_DATA0 | CH32_USBHS_ISO_RES;
    } else {
        ENDP_RX_CTRL(ep) = (ENDP_RX_CTRL(ep) & ~USBHS_UEP_R_RES_MASK) | USBHS_UEP_R_RES_ACK;
    }
}

/* 暂停接收（NAK）。同步 OUT 一般不用它：同步端点丢一包就永远补不回来。 */
static void ch32_usbhs_ep_out_idle(uint8_t ep)
{
    if (ep && ch32_usbhs_iso_out(ep)) {
        ENDP_RX_CTRL(ep) = USBHS_UEP_R_TOG_DATA0 | USBHS_UEP_R_RES_NAK;
    } else {
        ENDP_RX_CTRL(ep) = (ENDP_RX_CTRL(ep) & ~USBHS_UEP_R_RES_MASK) | USBHS_UEP_R_RES_NAK;
    }
}

/*
 * 把所有端点拉回"未挂载 + 未使用"的初始状态，并让 EP0 就位等 SETUP。
 * 总线复位（UIF_DETECT）必须调用：
 *   CherryUSB 的 core 在复位时只重开 EP0，其余端点的 ENDP_CONFIG / ENDP_TYPE
 *   不会被动过。不清就会带着上一轮枚举的端点配置进入下一轮。
 */
static void ch32_usbhs_hw_reset(uint8_t busid)
{
    struct ch32_usbhs_udc *udc = &g_ch32_usbhs_udc[busid];

    for (uint8_t ep = 1; ep < CONFIG_USBDEV_EP_NUM; ep++) {
        ENDP_TX_LEN(ep) = 0;
        ENDP_TX_CTRL(ep) = USBHS_UEP_T_TOG_DATA0 | USBHS_UEP_T_RES_NAK;
        ENDP_RX_CTRL(ep) = USBHS_UEP_R_TOG_DATA0 | USBHS_UEP_R_RES_NAK;
        ENDP_MAX_LEN(ep) = 0;
        udc->ep_in[ep].ep_mps = 0;
        udc->ep_in[ep].xfer_len = 0;
        udc->ep_in[ep].actual_xfer_len = 0;
        udc->ep_out[ep].ep_mps = 0;
        udc->ep_out[ep].xfer_len = 0;
        udc->ep_out[ep].actual_xfer_len = 0;
    }

    USBHSD->ENDP_CONFIG = USBHS_UEP0_T_EN | USBHS_UEP0_R_EN;
    USBHSD->ENDP_TYPE = 0;

    udc->dev_addr = 0;
    udc->dev_addr_pending = 0;
    udc->ep0_out_ptr = NULL;
    udc->ep_in[0].xfer_len = 0;
    udc->ep_in[0].actual_xfer_len = 0;
    udc->ep_out[0].xfer_len = 0;
    udc->ep_out[0].actual_xfer_len = 0;

    USBHSD->UEP0_DMA = (uint32_t)udc->ep0_dma;
    USBHSD->UEP0_MAX_LEN = 64;
    ENDP_TX_LEN(0) = 0;
    /* EP0 的翻转由软件管（见 handle_ep0_in）：这里先摆成"等 SETUP" */
    ENDP_TX_CTRL(0) = USBHS_UEP_T_TOG_DATA1 | USBHS_UEP_T_RES_NAK;
    ENDP_RX_CTRL(0) = USBHS_UEP_R_TOG_DATA1 | USBHS_UEP_R_RES_ACK;
}

int usb_dc_init(uint8_t busid)
{
    usb_dc_low_level_init(busid);

    /* 沁恒的时序：清 SIE、复位、放开 PHY 挂起，再开设备 */
    USBHSD->CONTROL = USBHS_UC_CLR_ALL | USBHS_UC_RESET_SIE;
    USBHSD->HOST_CTRL = 0;
    Delay_Us(10);
    USBHSD->CONTROL = 0;

    /* PHY_SUSPENDM 在 HOST_CTRL 里，不在 CONTROL 里 */
    USBHSD->HOST_CTRL = USBHS_UH_PHY_SUSPENDM;
    USBHSD->CONTROL = USBHS_UC_DMA_EN | USBHS_UC_INT_BUSY | USBHS_UC_SPEED_HIGH;

    USBHSD->DEV_AD = 0;
    USBHSD->BUF_MODE = 0; /* 单缓冲 */

    ch32_usbhs_hw_reset(busid);

    /* 把上一次残留的标志清干净，再开中断源。
     * UIE_ISO_ACT 默认不使能（见文件头 CH32_USBHS_ISO_ACT_INT 的说明）。 */
    USBHSD->INT_FG = 0xFF;
    USBHSD->INT_EN = USBHS_UIE_SETUP_ACT | USBHS_UIE_TRANSFER |
                     USBHS_UIE_DETECT | USBHS_UIE_SUSPEND;
#if CH32_USBHS_ISO_ACT_INT
    USBHSD->INT_EN |= USBHS_UIE_ISO_ACT;
#endif

    /* 设备上拉 —— 主机就是在这一步看到 D+ 的 */
    USBHSD->CONTROL |= USBHS_UC_DEV_PU_EN;

    return 0;
}

int usb_dc_deinit(uint8_t busid)
{
    USBHSD->INT_EN = 0;
    USBHSD->CONTROL = USBHS_UC_CLR_ALL | USBHS_UC_RESET_SIE;
    USBHSD->CONTROL = 0;
    USBHSD->HOST_CTRL = 0;
    usb_dc_low_level_deinit(busid);
    return 0;
}

int usbd_set_address(uint8_t busid, const uint8_t addr)
{
    struct ch32_usbhs_udc *udc = &g_ch32_usbhs_udc[busid];

    /* 地址要等 SET_ADDRESS 的状态阶段（EP0 的 IN 令牌）走完再写进 DEV_AD，
     * 否则状态阶段用的是新地址，主机会收不到。 */
    udc->dev_addr = addr;
    udc->dev_addr_pending = 1;
    return 0;
}

int usbd_set_remote_wakeup(uint8_t busid)
{
    (void)busid;
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
    struct ch32_usbhs_udc *udc = &g_ch32_usbhs_udc[busid];
    uint8_t epid = USB_EP_GET_IDX(ep->bEndpointAddress);
    uint8_t ep_type = USB_GET_ENDPOINT_TYPE(ep->bmAttributes);
    uint16_t ep_mps = USB_GET_MAXPACKETSIZE(ep->wMaxPacketSize);
    uint8_t is_iso = (ep_type == USB_ENDPOINT_TYPE_ISOCHRONOUS) ? 1 : 0;

    if (epid >= CONFIG_USBDEV_EP_NUM) {
        return -1;
    }

    if (epid == 0) {
        /* EP0：包长 64，翻转软件管，不要 TOG_AUTO（会和 handle_ep0_in 打架） */
        udc->ep_in[0].ep_mps = ep_mps;
        udc->ep_out[0].ep_mps = ep_mps;
        udc->ep_in[0].xfer_len = 0;
        udc->ep_out[0].xfer_len = 0;
        USBHSD->UEP0_MAX_LEN = ep_mps ? ep_mps : 64;
        USBHSD->UEP0_DMA = (uint32_t)udc->ep0_dma;
        ENDP_TX_LEN(0) = 0;
        ENDP_TX_CTRL(0) = USBHS_UEP_T_TOG_DATA1 | USBHS_UEP_T_RES_NAK;
        ENDP_RX_CTRL(0) = USBHS_UEP_R_TOG_DATA1 | USBHS_UEP_R_RES_ACK;
        return 0;
    }

    if (USB_EP_DIR_IS_IN(ep->bEndpointAddress)) {
        udc->ep_in[epid].ep_mps = ep_mps;
        udc->ep_in[epid].xfer_len = 0;
        udc->ep_in[epid].actual_xfer_len = 0;
        USBHSD->ENDP_CONFIG |= ENDP_T_EN_BIT(epid);
        ENDP_MAX_LEN(epid) = ep_mps;
        ENDP_TX_LEN(epid) = 0;
        if (is_iso) {
            USBHSD->ENDP_TYPE |= ENDP_T_TYPE_BIT(epid);
            /* 同步端点固定 DATA0，绝不 TOG_AUTO —— 理由见文件头 (b) */
            ENDP_TX_CTRL(epid) = USBHS_UEP_T_TOG_DATA0 | USBHS_UEP_T_RES_NAK;
        } else {
            USBHSD->ENDP_TYPE &= ~ENDP_T_TYPE_BIT(epid);
            ENDP_TX_CTRL(epid) = USBHS_UEP_T_TOG_AUTO | USBHS_UEP_T_RES_NAK;
        }
    } else {
        udc->ep_out[epid].ep_mps = ep_mps;
        udc->ep_out[epid].xfer_len = 0;
        udc->ep_out[epid].actual_xfer_len = 0;
        USBHSD->ENDP_CONFIG |= ENDP_R_EN_BIT(epid);
        ENDP_MAX_LEN(epid) = ep_mps;
        if (is_iso) {
            USBHSD->ENDP_TYPE |= ENDP_R_TYPE_BIT(epid);
            ENDP_RX_CTRL(epid) = USBHS_UEP_R_TOG_DATA0 | USBHS_UEP_R_RES_NAK;
        } else {
            USBHSD->ENDP_TYPE &= ~ENDP_R_TYPE_BIT(epid);
            ENDP_RX_CTRL(epid) = USBHS_UEP_R_TOG_AUTO | USBHS_UEP_R_RES_NAK;
        }
    }

    return 0;
}

int usbd_ep_close(uint8_t busid, const uint8_t ep)
{
    struct ch32_usbhs_udc *udc = &g_ch32_usbhs_udc[busid];
    uint8_t epid = USB_EP_GET_IDX(ep);

    if (epid == 0 || epid >= CONFIG_USBDEV_EP_NUM) {
        return -1;
    }

    if (USB_EP_DIR_IS_IN(ep)) {
        ENDP_TX_LEN(epid) = 0;
        ENDP_TX_CTRL(epid) = USBHS_UEP_T_TOG_DATA0 | USBHS_UEP_T_RES_NAK;
        USBHSD->ENDP_TYPE &= ~ENDP_T_TYPE_BIT(epid);
        USBHSD->ENDP_CONFIG &= ~ENDP_T_EN_BIT(epid);
        udc->ep_in[epid].xfer_len = 0;
    } else {
        ENDP_RX_CTRL(epid) = USBHS_UEP_R_TOG_DATA0 | USBHS_UEP_R_RES_NAK;
        USBHSD->ENDP_TYPE &= ~ENDP_R_TYPE_BIT(epid);
        USBHSD->ENDP_CONFIG &= ~ENDP_R_EN_BIT(epid);
        udc->ep_out[epid].xfer_len = 0;
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
    struct ch32_usbhs_udc *udc = &g_ch32_usbhs_udc[busid];
    uint8_t epid = USB_EP_GET_IDX(ep);
    uint8_t is_iso = USB_EP_DIR_IS_OUT(ep) ? ch32_usbhs_iso_out(epid) : ch32_usbhs_iso_in(epid);

    (void)busid;
    /* 清 halt 之后：翻转位复位（非同步端点仍交给 TOG_AUTO），端点回到"未挂载"。
     * 如果还有一笔没完成的接收，就继续挂着（否则主机的重传会一直 NAK 下去）。 */
    if (USB_EP_DIR_IS_OUT(ep)) {
        uint8_t tog = (epid && !is_iso) ? USBHS_UEP_R_TOG_AUTO : USBHS_UEP_R_TOG_DATA0;
        ENDP_RX_CTRL(epid) = tog | (udc->ep_out[epid].xfer_len ? USBHS_UEP_R_RES_ACK : USBHS_UEP_R_RES_NAK);
    } else {
        uint8_t tog = (epid && !is_iso) ? USBHS_UEP_T_TOG_AUTO : USBHS_UEP_T_TOG_DATA0;
        ENDP_TX_CTRL(epid) = tog | USBHS_UEP_T_RES_NAK;
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
    struct ch32_usbhs_udc *udc = &g_ch32_usbhs_udc[busid];
    uint8_t ep_idx = USB_EP_GET_IDX(ep);
    uint8_t is_iso = ch32_usbhs_iso_in(ep_idx);
    uint16_t mps = udc->ep_in[ep_idx].ep_mps;
    uint16_t tx_len;

    if (!data && data_len) {
        return -1;
    }
    if (data && ((uint32_t)data & 0x03)) {
        return -3;
    }
    /* 非同步端点必须处于 NAK 才能重新挂载（否则上一包还没发完）。
     * 同步端点允许随时刷新：反馈端点本身就是"每次完成就换一包新的"，
     * 它不做握手，刷新不会影响已经上线的那一包。 */
    if (!is_iso && (ENDP_TX_CTRL(ep_idx) & USBHS_UEP_T_RES_MASK) != USBHS_UEP_T_RES_NAK) {
        return -4;
    }

    tx_len = (uint16_t)MIN(data_len, (uint32_t)(mps ? mps : data_len));

    udc->ep_in[ep_idx].xfer_len = data_len;
    udc->ep_in[ep_idx].actual_xfer_len = 0;

    if (ep_idx == 0) {
        /* EP0：搬进 port 自己的 DMA 缓冲，UEP0_DMA 永远不动 */
        if (tx_len) {
            memcpy(udc->ep0_dma, data, tx_len);
        }
    } else if (data) {
        ENDP_TX_DMA(ep_idx) = (uint32_t)data;
    }

    ENDP_TX_LEN(ep_idx) = tx_len;
    ch32_usbhs_ep_in_arm(ep_idx);
    return 0;
}

int usbd_ep_start_read(uint8_t busid, const uint8_t ep, uint8_t *data, uint32_t data_len)
{
    struct ch32_usbhs_udc *udc = &g_ch32_usbhs_udc[busid];
    uint8_t ep_idx = USB_EP_GET_IDX(ep);
    uint16_t mps = udc->ep_out[ep_idx].ep_mps;

    if (!data && data_len) {
        return -1;
    }
    if (data && ((uint32_t)data & 0x03)) {
        return -3;
    }

    udc->ep_out[ep_idx].xfer_len = data_len;
    udc->ep_out[ep_idx].actual_xfer_len = 0;

    if (ep_idx == 0) {
        /* EP0 的数据落在 ep0_dma 里，收到之后由 handle_ep0_out 搬回去 */
        udc->ep0_out_ptr = data;
    } else if (data) {
        ENDP_RX_DMA(ep_idx) = (uint32_t)data;
        /* 硬件按 UEPn_MAX_LEN 收；把上限收到本次请求长度，避免主机发超长包写穿缓冲。
         * 注意 UEPn_MAX_LEN 收发共用，同索引两个方向包长不同时必须用不同索引。
         * 0 长度请求不动这个上限，免得把端点收成"什么都收不了"。 */
        if (data_len) {
            ENDP_MAX_LEN(ep_idx) = (uint16_t)MIN(data_len, (uint32_t)(mps ? mps : data_len));
        }
    }

    ch32_usbhs_ep_out_arm(ep_idx);
    return 0;
}

/* -------------------------------------------------------------------------
 * 传输完成
 * ------------------------------------------------------------------------- */

/* EP0 IN：数据阶段续包 + 状态阶段复位 */
static void handle_ep0_in(uint8_t busid)
{
    struct ch32_usbhs_udc *udc = &g_ch32_usbhs_udc[busid];

    if (udc->setup.bmRequestType & 0x80) {
        /* 数据阶段：翻转 DATA0/DATA1，然后先 NAK，交给 core 决定发不发下一包 */
        ENDP_TX_CTRL(0) ^= USBHS_UEP_T_TOG_DATA1;
        ENDP_TX_CTRL(0) = (ENDP_TX_CTRL(0) & ~USBHS_UEP_T_RES_MASK) | USBHS_UEP_T_RES_NAK;

        if (udc->ep_in[0].xfer_len > udc->ep_in[0].ep_mps) {
            udc->ep_in[0].xfer_len -= udc->ep_in[0].ep_mps;
            udc->ep_in[0].actual_xfer_len += udc->ep_in[0].ep_mps;
            usbd_event_ep_in_complete_handler(busid, 0x80, udc->ep_in[0].actual_xfer_len);
        } else {
            udc->ep_in[0].actual_xfer_len += udc->ep_in[0].xfer_len;
            udc->ep_in[0].xfer_len = 0;
            usbd_event_ep_in_complete_handler(busid, 0x80, udc->ep_in[0].actual_xfer_len);
        }
    } else {
        /* 无数据请求的 IN 状态阶段：重新摆好等 SETUP */
        ENDP_TX_CTRL(0) = USBHS_UEP_T_TOG_DATA1 | USBHS_UEP_T_RES_NAK;
        ENDP_RX_CTRL(0) = USBHS_UEP_R_TOG_DATA1 | USBHS_UEP_R_RES_ACK;
    }
}

static void handle_ep0_out(uint8_t busid)
{
    struct ch32_usbhs_udc *udc = &g_ch32_usbhs_udc[busid];
    uint32_t read_count = USBHSD->RX_LEN;

    if (read_count > udc->ep_out[0].xfer_len) {
        read_count = udc->ep_out[0].xfer_len;
    }

    if (read_count && udc->ep0_out_ptr) {
        memcpy(udc->ep0_out_ptr, udc->ep0_dma, read_count);
        /* 控制传输数据阶段每包都要翻转 DATA（第 1 包 DATA1、第 2 包 DATA0 …）。
         * 硬件在非 AUTO 模式下不自己翻转，漏掉这一步会让第 2 包 TOG_OK=0 被丢掉，
         * 于是 wLength > 64 的控制 OUT（下载）传输走到一半就卡住。 */
        ENDP_RX_CTRL(0) ^= USBHS_UEP_R_TOG_DATA1;
    }
    udc->ep_out[0].xfer_len -= read_count;

    /* 交给 core 的是"本包收了多少"，不是累计值：
     * usbd_event_ep0_out_complete_handler() 自己会推进缓冲指针和剩余长度。
     * 状态阶段 read_count=0，core 收到 nbytes=0 就知道一次控制传输结束了。 */
    usbd_event_ep_out_complete_handler(busid, 0x00, read_count);
}

static void handle_non_ep0_in(uint8_t busid, uint8_t epid)
{
    struct ch32_usbhs_udc *udc = &g_ch32_usbhs_udc[busid];

    ch32_usbhs_ep_in_idle(epid);

    if (udc->ep_in[epid].xfer_len > udc->ep_in[epid].ep_mps) {
        uint32_t write_count = MIN(udc->ep_in[epid].xfer_len, udc->ep_in[epid].ep_mps);

        udc->ep_in[epid].xfer_len -= udc->ep_in[epid].ep_mps;
        udc->ep_in[epid].actual_xfer_len += udc->ep_in[epid].ep_mps;

        ENDP_TX_LEN(epid) = (uint16_t)write_count;
        ENDP_TX_DMA(epid) += udc->ep_in[epid].ep_mps;
        ch32_usbhs_ep_in_arm(epid);
    } else {
        udc->ep_in[epid].actual_xfer_len += udc->ep_in[epid].xfer_len;
        udc->ep_in[epid].xfer_len = 0;
        usbd_event_ep_in_complete_handler(busid, 0x80 | epid, udc->ep_in[epid].actual_xfer_len);
    }
}

static void handle_non_ep0_out(uint8_t busid, uint8_t epid, uint32_t rx_len)
{
    struct ch32_usbhs_udc *udc = &g_ch32_usbhs_udc[busid];
    uint32_t read_count = MIN(rx_len, udc->ep_out[epid].xfer_len);
    uint8_t is_iso = ch32_usbhs_iso_out(epid);

    /* 非同步端点先 NAK，避免回调里重新挂载之前又来一包踩同一个缓冲。
     * 同步端点不能 NAK：丢一包就是永久丢失，而且它的 DMA 目标由上层立刻换新。 */
    if (!is_iso) {
        ch32_usbhs_ep_out_idle(epid);
    }

    udc->ep_out[epid].actual_xfer_len += read_count;
    udc->ep_out[epid].xfer_len -= read_count;

    /*
     * 同步端点：一个微帧就是一次"传输"，收到就上交（bInterval=1、mult=0 时
     * 每个微帧只有一包）。上层在回调里换缓冲、重新挂载下一包。
     * 非同步端点：按 CherryUSB 的语义，短包或收满才算一次传输完成。
     */
    if (is_iso || (read_count < udc->ep_out[epid].ep_mps) || (udc->ep_out[epid].xfer_len == 0)) {
        usbd_event_ep_out_complete_handler(busid, epid, udc->ep_out[epid].actual_xfer_len);
    } else {
        ENDP_RX_DMA(epid) += udc->ep_out[epid].ep_mps;
        ENDP_MAX_LEN(epid) = (uint16_t)MIN(udc->ep_out[epid].xfer_len, udc->ep_out[epid].ep_mps);
        ch32_usbhs_ep_out_arm(epid);
    }
}

void USBD_IRQHandler(uint8_t busid)
{
    struct ch32_usbhs_udc *udc = &g_ch32_usbhs_udc[busid];
    uint8_t intflag = USBHSD->INT_FG;
    uint8_t intst = USBHSD->INT_ST;

#if CH32_USBHS_ISO_ACT_INT
    if (intflag & USBHS_UIF_TRANSFER_MASK) {
#else
    if (intflag & USBHS_UIF_TRANSFER) {
#endif
        uint8_t token = intst & USBHS_UIS_TOKEN_MASK;
        uint8_t endp = intst & USBHS_UIS_ENDP_MASK;
        uint32_t rx_len = 0;

#if CH32_USBHS_ISO_ACT_INT
        /*
         * ISO_ACT 单独置位（TRANSFER 没置位）时，只有同步端点的完成才可能走这条路。
         * 如果它指向的不是同步端点，说明这一位是"状态位"而不是完成事件：
         * 把 token 改成一个不会命中任何分支的值，等于什么都不做。
         * （UIS_TOKEN_SETUP=0b11 不会命中 IN/OUT/SOF 三个分支）
         */
        if ((intflag & USBHS_UIF_ISO_ACT) && !(intflag & USBHS_UIF_TRANSFER) &&
            !ch32_usbhs_iso_in(endp) && !ch32_usbhs_iso_out(endp)) {
            token = USBHS_UIS_TOKEN_SETUP;
        }
#endif

        if (token == USBHS_UIS_TOKEN_OUT) {
            rx_len = USBHSD->RX_LEN; /* 必须在清标志之前锁存 */
        }

        /*
         * 非 EP0 的完成标志先清掉再跑用户回调：
         * 同步传输不受 UC_INT_BUSY 约束，如果我们在这里慢慢处理，下一包的完成
         * 会盖掉这一次的 INT_ST / RX_LEN。所以本包要用的信息已经先锁存到局部变量。
         * EP0 例外 —— EP0 受 INT_BUSY 保护，提前放行反而会让新的阶段插进来。
         */
        if (endp != 0) {
            USBHSD->INT_FG = intflag & USBHS_UIF_TRANSFER_MASK;
        }

        if (token == USBHS_UIS_TOKEN_IN) {
            if (endp == 0) {
                handle_ep0_in(busid);
                if (udc->dev_addr_pending) {
                    USBHSD->DEV_AD = udc->dev_addr;
                    udc->dev_addr_pending = 0;
                }
            } else {
                handle_non_ep0_in(busid, endp);
            }
        } else if (token == USBHS_UIS_TOKEN_OUT) {
            if (endp == 0) {
                handle_ep0_out(busid);
            } else if (ch32_usbhs_iso_out(endp) || (intst & USBHS_UIS_TOG_OK)) {
                /*
                 * 同步端点不看 TOG_OK：同步 OUT 的 PID 由主机固定为 DATA0，
                 * 这一位在同步端点上一旦为 0，包会被 ACK 掉却不上交，
                 * 而且这个分支不会重新挂 DMA —— 之后就每一包都从这里漏掉，
                 * 表现就是"回调只进一次"。非同步端点照旧要看。
                 */
                handle_non_ep0_out(busid, endp, rx_len);
            } else {
                /* 非同步端点翻转不匹配：重新 ACK，等主机重传 */
                ch32_usbhs_ep_out_arm(endp);
            }
        } else if (token == USBHS_UIS_TOKEN_SOF) {
            /* 设备模式下 SOF 以 token 形式出现在 TRANSFER 中断里（UIF_HST_SOF 是主机模式的）。
             * 高速下每 125us 一次。异步播放的反馈端点可以靠它刷新。 */
            usbd_event_sof_handler(busid);
        }

        if (endp == 0) {
            USBHSD->INT_FG = intflag & USBHS_UIF_TRANSFER_MASK;
        }
    } else if (intflag & USBHS_UIF_SETUP_ACT) {
        USBHSD->UEP0_DMA = (uint32_t)udc->ep0_dma;
        memcpy(&udc->setup, udc->ep0_dma, sizeof(udc->setup));
        ENDP_TX_CTRL(0) = USBHS_UEP_T_TOG_DATA1 | USBHS_UEP_T_RES_NAK;
        ENDP_RX_CTRL(0) = USBHS_UEP_R_TOG_DATA1 | USBHS_UEP_R_RES_NAK;
        udc->ep_in[0].xfer_len = 0;
        udc->ep_in[0].actual_xfer_len = 0;
        udc->ep_out[0].xfer_len = 0;
        udc->ep_out[0].actual_xfer_len = 0;
        udc->ep0_out_ptr = NULL;

        usbd_event_ep0_setup_complete_handler(busid, (uint8_t *)&udc->setup);

        USBHSD->INT_FG = USBHS_UIF_SETUP_ACT;
    } else if (intflag & USBHS_UIF_DETECT) {
        /* 总线复位 */
        USBHSD->INT_FG = USBHS_UIF_DETECT;
        ch32_usbhs_hw_reset(busid);
        usbd_event_reset_handler(busid);
    } else if (intflag & USBHS_UIF_SUSPEND) {
        USBHSD->INT_FG = USBHS_UIF_SUSPEND;
        if (USBHSD->MIS_ST & USBHS_UMS_SUSPEND) {
            usbd_event_suspend_handler(busid);
        } else {
            usbd_event_resume_handler(busid);
        }
    } else {
        /* 兜底：把没人认领的标志清掉，绝不让 INT_FG 挂着不该挂的位造成中断风暴 */
        USBHSD->INT_FG = intflag;
    }

    /*
     * 顺手清掉 ISO_ACT（写 1 清标志）。
     * 这一位是 USBHS 独有的"同步活动"标志，它的中断（UIE_ISO_ACT）我们故意不使能
     * —— 使能后会反复重入。但它一旦挂住，同步端点的完成事件会退不掉，
     * 所以每次进中断都写一次，代价只有一个寄存器写。
     */
    USBHSD->INT_FG = USBHS_UIF_ISO_ACT;
}
