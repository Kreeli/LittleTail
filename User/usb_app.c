#include "debug.h"
#include "usbd_core.h"
#include "usb_app.h"
#include "usbd_desc.h"

#define BUF_SIZE 512

/* USB DMA 要求 4 字节对齐 */
static uint8_t rx_buf[BUF_SIZE];
static uint8_t tx_buf[BUF_SIZE];
static volatile uint8_t tx_busy = 0;

const char* USB_str = "Hachimi";
static struct usbd_interface intf_cmd,intf_data;
static struct usbd_endpoint ep_notify = {
    .ep_addr = EP_CDC_INT,
    .ep_cb = CDC_Notified_callback
},
ep_in = {
    .ep_addr = EP_CDC_IN,
    .ep_cb = CDC_in_callback
},
ep_out = {
    .ep_addr = EP_CDC_OUT,
    .ep_cb = CDC_out_callback
};

static const struct usb_descriptor s_desc = {//回调函数的结构体
    .device_descriptor_callback = get_dev,
    .config_descriptor_callback = get_cfg,
    .string_descriptor_callback = get_str
};



void usb_dc_low_level_init(uint8_t busid){
    USBHS_RCC_init();
    NVIC_InitTypeDef NVIC_S = {
        .NVIC_IRQChannel = USBHS_IRQn,
        .NVIC_IRQChannelCmd = ENABLE,
        .NVIC_IRQChannelPreemptionPriority = 0,
        .NVIC_IRQChannelSubPriority = 1
    };
    NVIC_Init(&NVIC_S);
}

void USBHS_RCC_init(void){
    /* 与 WCH USBHS 示例一致：48MHz 来自 USB PHY，而不是 PLLCLK */
    RCC_USBCLK48MConfig( RCC_USBCLK48MCLKSource_USBPHY );
    RCC_USBHSPLLCLKConfig( RCC_HSBHSPLLCLKSource_HSE );
    RCC_USBHSConfig( RCC_USBPLL_Div2 );
    RCC_USBHSPLLCKREFCLKConfig( RCC_USBHSPLLCKREFCLK_4M );
    RCC_USBHSPHYPLLALIVEcmd( ENABLE );

    Delay_Ms(10);   /* 等待 HS PHY PLL 锁定 */
    RCC_AHBPeriphClockCmd( RCC_AHBPeriph_USBHS, ENABLE );
}

void USBHS_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void USBHS_IRQHandler(void){
    USBD_IRQHandler(0);//进协议栈
    return;
}

const uint8_t *get_dev(uint8_t speed){
    return device_desc;

}
const uint8_t *get_cfg(uint8_t speed){
    return conf_desc;
}

const char *get_str(uint8_t speed, uint8_t index)
{
    (void)speed;
    switch (index) {
    case 0: return "\x09\x04"; /* LANGID 0x0409 */
    case 1: return USB_str;
    case 2: return "LittleTail";
    case 3: return "LittleTail";
    default: return NULL;
    }
}

void USB_CDC_init(void){
    usbd_desc_register(0,&s_desc);//注册回调函数

    usbd_add_interface(0,usbd_cdc_acm_init_intf(0,&intf_cmd));//按描述符顺序添加接口
    usbd_add_interface(0,&intf_data);

    usbd_add_endpoint(0,&ep_notify);//按描述符顺序添加端点
    usbd_add_endpoint(0,&ep_in);
    usbd_add_endpoint(0,&ep_out);

    usbd_initialize(0, USBHS_BASE, usb_event_handler);
}

void CDC_WriteBlocking(uint8_t* buf,int size){
    if(size <= 0 | buf == NULL)
        return;
    while(tx_busy)
        continue;
    memcpy(tx_buf,buf,size);
    tx_busy = 1;
    if (usbd_ep_start_write(0, EP_CDC_IN, tx_buf, size) != 0) {
        tx_busy = 0;   // 没发出去，立刻放行
        return;
    }
}


void CDC_Notified_callback(uint8_t busid, uint8_t ep, uint32_t nbytes){
    return;
}

void CDC_in_callback(uint8_t busid, uint8_t ep, uint32_t nbytes){
    (void)busid; (void)ep; (void)nbytes;
    tx_busy = 0;
    GPIO_SetBits(GPIOC,GPIO_Pin_9);
    return;
}

void CDC_out_callback(uint8_t busid, uint8_t ep, uint32_t nbytes){
    (void)busid; (void)ep;
    return;
}

void usb_event_handler(uint8_t busid, uint8_t event)
{
    (void)busid;
    switch (event) {
    case USBD_EVENT_INIT:
        break;
    case USBD_EVENT_RESET:
        break;
    case USBD_EVENT_CONNECTED:
        break;
    case USBD_EVENT_CONFIGURED:
        usbd_ep_start_read(0,EP_CDC_OUT,rx_buf,BUF_SIZE);
        break;
    default:
        break;
    }
}

