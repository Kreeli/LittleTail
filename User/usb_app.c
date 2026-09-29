#include "debug.h"
#include "usbd_core.h"
#include "usb_app.h"
#include "usbd_desc.h"
#include "ES9018.h"
#define BUF_SIZE 512

/* USB DMA 要求 4 字节对齐 */
static uint8_t rx_buf[BUF_SIZE];
static uint8_t tx_buf[BUF_SIZE];
static volatile uint8_t tx_busy = 0;

static struct usbd_interface cdc_intf_cmd,cdc_intf_data,audio_intf_cmd,audio_intf_data;
static struct usbd_endpoint ep_cdc_notify = {
    .ep_addr = EP_CDC_INT,
    .ep_cb = CDC_Notified_callback
},
ep_cdc_in = {
    .ep_addr = EP_CDC_IN,
    .ep_cb = CDC_in_callback
},
ep_cdc_out = {
    .ep_addr = EP_CDC_OUT,
    .ep_cb = CDC_out_callback
},
ep_audio_data = {
    .ep_addr = EP_AUDIO_OUT,
    .ep_cb = Audio_in_callback
},
ep_audio_feedback = {
    .ep_addr = EP_AUDIO_FEEDBACK,
    .ep_cb = Audio_feedback_callback
};

static struct audio_entity_info audio_table[] = {
    { AUDIO_CONTROL_INPUT_TERMINAL,  0x01, EP_AUDIO_OUT },
    { AUDIO_CONTROL_OUTPUT_TERMINAL, 0x02, EP_AUDIO_OUT },
};


static const struct usb_descriptor s_desc = {//回调函数的结构体
    .device_descriptor_callback = get_dev,
    .config_descriptor_callback = get_cfg,
    .string_descriptor_callback = get_str
};

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
    case 1: return "hachimi";
    case 2: return "LittleTail";
    case 3: return "hachimi LittleTail";
    default: return NULL;
    }
}

void usb_dc_low_level_init(uint8_t busid){//cherryUSB的HOOK
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

void USB_init(void){
    usbd_desc_register(0,&s_desc);//注册回调函数

    usbd_add_interface(0,usbd_cdc_acm_init_intf(0,&cdc_intf_cmd));//按描述符顺序添加接口
    usbd_add_interface(0,&cdc_intf_data);
    usbd_add_interface(0,usbd_audio_init_intf(0,&audio_intf_cmd,0x0100,audio_table,2));//按描述符顺序添加接口
    usbd_add_interface(0,&audio_intf_data);

    usbd_add_endpoint(0,&ep_cdc_notify);//按描述符顺序添加端点
    usbd_add_endpoint(0,&ep_cdc_in);
    usbd_add_endpoint(0,&ep_cdc_out);
    usbd_add_endpoint(0,&ep_audio_data);
    usbd_add_endpoint(0,&ep_audio_feedback);

    usbd_initialize(0, USBHS_BASE, usb_event_handler);
}

void USBHS_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void USBHS_IRQHandler(void){
    USBD_IRQHandler(0);//进协议栈
    return;
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
    return;
}

void CDC_out_callback(uint8_t busid, uint8_t ep, uint32_t nbytes){
    (void)busid; (void)ep;
    return;
}
void Audio_in_callback(uint8_t busid, uint8_t ep, uint32_t nbytes){
    GPIO_SetBits(GPIOC,GPIO_Pin_9);
    return;
}

void Audio_feedback_callback(uint8_t busid, uint8_t ep, uint32_t nbytes){
    GPIO_SetBits(GPIOC,GPIO_Pin_9);
    return;
}


void usbd_audio_set_volume(uint8_t busid, uint8_t ep, uint8_t ch, float volume_db)
{
    (void)busid;
    (void)ep;
    (void)ch;
    (void)volume_db;
}

float usbd_audio_get_volume(uint8_t busid, uint8_t ep, uint8_t ch)
{
    (void)busid;
    (void)ep;
    (void)ch;

    return 0.0f;
}

void usbd_audio_get_volume_range(uint8_t busid, uint8_t ep, uint8_t ch, float *min, float *max, float *res)
{
    (void)busid;
    (void)ep;
    (void)ch;

    *min = -100.0f;
    *max = 0.0f;
    *res = 1.0f;
}

void usbd_audio_set_mute(uint8_t busid, uint8_t ep, uint8_t ch, bool mute)
{
    (void)busid;
    (void)ep;
    (void)ch;
    (void)mute;

}

bool usbd_audio_get_mute(uint8_t busid, uint8_t ep, uint8_t ch)
{
    (void)busid;
    (void)ep;
    (void)ch;

    return 0;
}

void usbd_audio_set_sampling_freq(uint8_t busid, uint8_t ep, uint32_t sampling_freq)
{
    (void)busid;
    (void)ep;
    (void)sampling_freq;
}

uint32_t usbd_audio_get_sampling_freq(uint8_t busid, uint8_t ep)
{
    (void)busid;
    (void)ep;

    return 48000;
}

void usbd_audio_get_sampling_freq_table(uint8_t busid, uint8_t ep, uint8_t **sampling_freq_table)
{
    static uint8_t g_audio_fs_table[] = {
        0xE0, 0x2E, 0x00,  /* 12000 */
        0x80, 0x5D, 0x00,  /* 24000 */
        0x80, 0xBB, 0x00,  /* 48000 */
        0x00, 0x77, 0x01   /* 96000 */
    };
    *sampling_freq_table = g_audio_fs_table;
}

void usbd_audio_open(uint8_t busid, uint8_t intf)
{
    (void)busid;
    (void)intf;
}

void usbd_audio_close(uint8_t busid, uint8_t intf)
{
    (void)busid;
    (void)intf;
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

