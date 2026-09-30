#include "debug.h"
#include "usbd_core.h"
#include "usb_app.h"
#include "usbd_desc.h"
#include "ES9018.h"
#include "assert.h"
#include "Syscfg.h"
#define BUF_SIZE 512
#define AUDIO_FRAME_SIZE (BINTERVAL * 12 * 8) //96k采样率一个微帧12*4*2 = 96bytes

/*
 * 音频OUT端点的接收缓冲：必须按**端点最大包长**开，不能只按"一包的两倍"开。
 * USBHS 硬件是拿着 UEPn_MAX_LEN（= 描述符里的 wMaxPacketSize = AUDIO_MAXSIZE）
 * 往这个地址 DMA 的，而且主机允许发满这个长度；缓冲比它小就会被写穿，
 * 后面的 i2s_tx_buf 和各个接口结构体会被音频数据踩掉。
 * 另外 USB DMA 要求 4 字节对齐。
 */
#define AUDIO_RX_SIZE     	AUDIO_MAXSIZE

#define I2S_BUF_SIZE     	(AUDIO_FRAME_SIZE * 8u)//I2S缓冲区存放的数据量 96k采样率下，会有2ms延迟

/* USB DMA 要求 4 字节对齐 */
static uint8_t  rx_buf[BUF_SIZE],
                tx_buf[BUF_SIZE];

static __attribute__((aligned(4))) uint8_t USB_audio_buf[2][AUDIO_RX_SIZE] = {};//两个缓冲区，互相放
uint8_t* current_USB_audio_arr = (uint8_t*)USB_audio_buf;//指示当前的音频数组

static uint16_t i2s_tx_buf[I2S_BUF_SIZE];//I2S缓冲数组
static uint16_t i2s_read_idx = 0,//缓冲区的读指针
                i2s_write_idx = 0;//缓冲区的写指针

static volatile uint8_t tx_busy = 0;//CDC传输正忙
bool audio_open = false;
static bool g_audio_first_open= false;

/* 反馈端点(设备->主机)的数据缓冲：高速下是 16.16 定点，单位 = 每微帧样点数。
 * 必须 4 字节对齐 —— usbd_ep_start_write() 对未对齐的地址直接返回错误，
 * 反馈一包都发不出去。 */
static __attribute__((aligned(4))) uint8_t audio_fb_buf[4];

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
    .ep_cb = Audio_out_callback
},
ep_audio_feedback = {
    .ep_addr = EP_AUDIO_FEEDBACK,
    .ep_cb = Audio_feedback_callback
};

static struct audio_entity_info audio_table[] = {
    { AUDIO_CONTROL_CLOCK_SOURCE,  0x01, EP_AUDIO_OUT },
    { AUDIO_CONTROL_INPUT_TERMINAL, 0x02, EP_AUDIO_OUT},
    { AUDIO_CONTROL_FEATURE_UNIT,  0x03, EP_AUDIO_OUT },
    { AUDIO_CONTROL_OUTPUT_TERMINAL, 0x04, EP_AUDIO_FEEDBACK}
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
    usbd_add_interface(0,usbd_audio_init_intf(0,&audio_intf_cmd,0x0200,audio_table,4));//按描述符顺序添加接口
    usbd_add_interface(0,usbd_audio_init_intf(0,&audio_intf_data,0x0200,audio_table,4));

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

static SAMPLE_RATE FS = FS_96000;


static uint8_t g_audio_fs_table[] = {
    WBVAL((0x0003)),
    DBVAL(24000),DBVAL(24000),DBVAL(0),
    DBVAL(48000),DBVAL(48000),DBVAL(0),
    DBVAL(96000),DBVAL(96000),DBVAL(0),
};

bool g_update = false, //参数需要更新
     g_mute = false;   //静音
uint8_t g_volumn_db = 0;

void usbd_audio_set_volume(uint8_t busid, uint8_t ep, uint8_t ch, float volume_db)
{
    (void)busid;
    (void)ep;
    (void)ch;
    g_volumn_db = (uint8_t)-volume_db;
    g_update = true;
}

float usbd_audio_get_volume(uint8_t busid, uint8_t ep, uint8_t ch)
{
    (void)busid;
    (void)ep;
    (void)ch;

    return g_volumn_db;
}

void usbd_audio_get_volume_range(uint8_t busid, uint8_t ep, uint8_t ch, float *min, float *max, float *res)
{
    (void)busid;
    (void)ep;
    (void)ch;

    *min = -255.0f;
    *max = 0.0f;
    *res = 1.0f;
}

void usbd_audio_set_mute(uint8_t busid, uint8_t ep, uint8_t ch, bool mute)
{
    (void)busid;
    (void)ep;
    (void)ch;
    g_mute = mute;
    g_update = true;
}

bool usbd_audio_get_mute(uint8_t busid, uint8_t ep, uint8_t ch)
{
    (void)busid;
    (void)ep;
    (void)ch;
    return g_mute;
}

void usbd_audio_set_sampling_freq(uint8_t busid, uint8_t ep, uint32_t sampling_freq)
{
    (void)busid;
    (void)ep;
    (void)sampling_freq;
    switch (sampling_freq) {
        case 24000:
            FS = FS_24000;
            break;
        case 48000:
            FS = FS_48000;
            break;
        case 96000:
            FS = FS_96000;
            break;
        default:
            break;
    }
    g_update = true;
    return;
}

uint32_t usbd_audio_get_sampling_freq(uint8_t busid, uint8_t ep)
{
    (void)busid;
    (void)ep;

    return FS;
}

void usbd_audio_get_sampling_freq_table(uint8_t busid, uint8_t ep, uint8_t **sampling_freq_table)
{   
    *sampling_freq_table = g_audio_fs_table;
}

/*
 * 反馈端点：UAC2 异步播放(AS 端点的 bmAttributes=0x05)时，主机靠这个端点
 * 送回来的速率值决定"下一个微帧发多少个样点"。**端点里必须有数据**，
 * 主机每次来取都要能取到；一直 NAK 的话主机拿不到速率，
 * 异步流就没法正常维持（这也是之前缓冲区一直漂移/断流的原因之一）。
 *
 * 高速下格式是 16.16 定点，单位 = 每微帧样点数：
 *   96k -> 12.0 -> 12 * 65536 = 786432    48k -> 393216    24k -> 196608
 */
static void Audio_SendFeedback(uint8_t busid)
{
    uint32_t fb = ((uint32_t)FS << 16) / 8000u;   //8000 = 高速每秒的微帧数

    audio_fb_buf[0] = (uint8_t)(fb);
    audio_fb_buf[1] = (uint8_t)(fb >> 8);
    audio_fb_buf[2] = (uint8_t)(fb >> 16);
    audio_fb_buf[3] = (uint8_t)(fb >> 24);

    (void)usbd_ep_start_write(busid, EP_AUDIO_FEEDBACK, audio_fb_buf, sizeof(audio_fb_buf));
}

void usbd_audio_open(uint8_t busid, uint8_t intf)
{
    if(intf == AUDIO_AS_INTERFACE){
        usbd_ep_start_read(busid,EP_AUDIO_OUT,current_USB_audio_arr,AUDIO_RX_SIZE);
        audio_open = true;
        g_audio_first_open = true;
        i2s_write_idx = 0;
        i2s_read_idx = 0;
        Audio_SendFeedback(busid);   /* 反馈端点先挂上第一包，主机一取就有 */
    }
}

void usbd_audio_close(uint8_t busid, uint8_t intf)
{
    (void)busid;
    (void)intf;
        if(intf == AUDIO_AS_INTERFACE){
        current_USB_audio_arr = (uint8_t*)USB_audio_buf[0];
        audio_open = false;
        I2S2_DMA_Stop();
        GPIO_ResetBits(GPIOC,GPIO_Pin_8);
    }
    
}


void Audio_out_callback(uint8_t busid, uint8_t ep, uint32_t nbytes){
    static uint32_t frame_num = 0;//记录开启后，端点一共传输了多少包数据，在到达一半的I2S缓冲之后，不在自增
    uint16_t* src_pt = (uint16_t*)current_USB_audio_arr,
            * dest_pt = (uint16_t*)i2s_tx_buf;
    current_USB_audio_arr = (current_USB_audio_arr == (uint8_t*)USB_audio_buf[0])?USB_audio_buf[1]:USB_audio_buf[0];
    volatile int temp = usbd_ep_start_read(busid,EP_AUDIO_OUT,current_USB_audio_arr,AUDIO_RX_SIZE);
    if(g_audio_first_open){
        if(frame_num++ < I2S_BUF_SIZE / AUDIO_FRAME_SIZE/2){
            ;//啥也不做
        }
        else{
            g_audio_first_open = false;
            I2S2_DMA_Init(i2s_tx_buf, I2S_BUF_SIZE);
	        I2S2_DMA_Start();
            GPIO_SetBits(GPIOC,GPIO_Pin_8);
            frame_num = 0;
        }
    }
    //搬数据
    
    for(int i = 0;i<nbytes/2;i+=2){
        dest_pt[i2s_write_idx] = src_pt[i+1];
        dest_pt[i2s_write_idx+1] = src_pt[i];
        i2s_write_idx = (2  + i2s_write_idx)%I2S_BUF_SIZE;
    }
      
    return;
}

void Audio_feedback_callback(uint8_t busid, uint8_t ep, uint32_t nbytes){
    (void)ep;
    (void)nbytes;

    /* 发完一包立刻续下一包：只要主机来 poll，端点里永远有数据 */
    Audio_SendFeedback(busid);
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
    case USBD_EVENT_SOF:
        
        break;
    default:
        break;
    }
}

