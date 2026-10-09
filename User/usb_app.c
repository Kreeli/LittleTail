#include "debug.h"
#include "usbd_core.h"
#include "usb_app.h"
#include "usbd_desc.h"
#include "ES9018.h"
#include "assert.h"
#include "Syscfg.h"
#include "pid.h"
#include "string.h"
#include "stdlib.h"

#define BUF_SIZE 512
/* 每个微帧的音频字节数（48k=48、96k=96、24k=24）。只用于说明，别当定值用，
 * 实际每包长度看回调的 nbytes（主机会按反馈值抖动，偶尔多/少一帧）。 */
#define AUDIO_FRAME_SIZE (BINTERVAL * 12 * 8)

/*
 * 音频OUT端点的接收缓冲：必须按**端点最大包长**开，不能只按"一包的两倍"开。
 * USBHS 硬件是拿着 UEPn_MAX_LEN（= 描述符里的 wMaxPacketSize = AUDIO_MAXSIZE）
 * 往这个地址 DMA 的，而且主机允许发满这个长度；缓冲比它小就会被写穿，
 * 后面的 i2s_tx_buf 和各个接口结构体会被音频数据踩掉。
 * 另外 USB DMA 要求 4 字节对齐。
 */
#define AUDIO_RX_SIZE     	AUDIO_MAXSIZE

/* I2S 播放缓冲（半字数）：见 Syscfg.h，默认 1536 -> 48k 下 8ms、96k 下 4ms。
 * 这里用 int16_t 数组，所以 1536 个半字 = 3KB。 */
#define I2S_BUF_SIZE     	I2S_BUF_HALFWORDS

/* USB DMA 要求 4 字节对齐 */
static uint8_t  rx_buf[BUF_SIZE],
                tx_buf[BUF_SIZE];

static __attribute__((aligned(4))) uint8_t USB_audio_buf[2][AUDIO_RX_SIZE] = {};//两个缓冲区，互相放
uint8_t* current_USB_audio_arr = (uint8_t*)USB_audio_buf;//指示当前的音频数组

static int16_t i2s_tx_buf[I2S_BUF_SIZE];//I2S缓冲数组
static int16_t i2s_read_idx = 0,//缓冲区的读指针
                i2s_write_idx = 0;//缓冲区的写指针

static volatile uint8_t tx_busy = 0;//CDC传输正忙
bool audio_open = false,
     g_cdc_out = false;
static volatile uint16_t g_cdc_out_len = 0;   /* 上一次 CDC OUT 实际收到多少字节 */

/* ===========================================================================
 * 音频同步：把主机的送数速率锁到本机 I2S 的真实采样率上
 *
 * 为什么必须做：
 *   主机按它自己的 125us 微帧送数据，本机按 I2S 的 LRCK 播放。两边晶振不同，
 *   而且 I2S 是整数分频：fS = 160MHz/(64*N)，一定不是整 48000：
 *       48k: N=52 -> 48076.92Hz (+0.16%)      96k: N=26 -> 96153.85Hz
 *   所以"标称反馈值"必须按真实速率算，否则每秒固定差 76.9 帧（48k），
 *   缓冲迟早被抽干或撑爆 —— 这就是之前怎么调 PID 都不稳的根因。
 *
 * 三段结构：
 *   1) 标称（纯整数，不用浮点）
 *        每微帧帧数 = fS_real / 8000 = 312.5 / N
 *        16.16 定点  = 312.5*65536 / N = 20480000 / N
 *        48k(N=52) -> 393846 = 6.009615 帧/微帧
 *   2) 修正：PI 控制器，输入水位误差（半字），输出速率修正（帧/微帧）
 *   3) 反馈值 = 标称 + 修正，限幅后放在 s_fb_fixed 里，中断里原样发出去
 *
 * 水位怎么量（这两点是关键，之前错在这）：
 *   a) 必须是环形差 (write - played + N) % N，**不能用绝对差**：
 *      读指针一圈 = 缓冲区那么多半字，用绝对差在过零点附近符号会翻。
 *   b) 必须在**固定相位**采样：USB 数据是每微帧一次性写进来的（脉冲），
 *      DMA 是连续播的，瞬时水位是锯齿（峰峰值 ≈ 一包的半字数）。
 *      所以就在 ISO OUT 回调里、刚搬完数据之后量，每微帧一次、相位固定。
 * ========================================================================= */
/*
 * 目标水位 / 安全区：**按时间算，不按半字数算**，运行期随采样率更新。
 *   目标水位 = 4ms 的数据量；安全区 = 2ms。
 * 这样 48k 和 96k 的余量（以时间计）一样：48k -> 768/384 半字，
 * 96k -> 1538/769 半字。缓冲数组按最坏情况（96k 8ms）开，见 Syscfg.h。
 */
/*
 * 水位目标 / 安全区（毫秒）。**翻倍过**：4ms/2ms -> 8ms/4ms。
 * 为什么：主机（尤其安卓）在忙的时候会有几十 ms 的 URB 提交延迟，
 * 余量太小就会欠载。翻倍之后实测足够，且不增加可感延迟（还是毫秒级缓冲）。
 *
 * 注意 96k 下的上限：缓冲 3072 半字 = 8ms@96k，而目标水位夹取到"缓冲的一半"，
 * 所以 96k 实际水位是 4ms 而不是 8ms。48k 下缓冲 16ms、目标 8ms，不受影响。
 */
#define SYNC_TARGET_MS      8u
#define SYNC_MARGIN_MS      4u
static int32_t s_target_level = 768;    /* 运行期由 Audio_UpdateFsDependent() 更新 */
static int32_t s_safe_margin  = 384;
static volatile uint8_t s_stream_idle;  /* 1 = 主机不送数据了（流停了但接口没关） */
static uint8_t s_idle_ticks;            /* 连续多少个 tick 没收到包 */

#define SYNC_KP_DEFAULT     0.001    /* 实测可用的一组系数，也可以用串口发 p=/i=/d= 在线调 */
#define SYNC_KI_DEFAULT     0.0004
#define SYNC_KD_DEFAULT     0.0
#define SYNC_CORR_LIMIT     0.25     /* 修正限幅 ±0.25 帧/微帧 = ±2%，别让主机跑飞 */
#define SYNC_INTEG_LIMIT    1000.0   /* 积分限幅（单位=半字·秒，防饱和）。
                                      * 1000*Ki=0.03 帧/微帧=240 帧/s，足够补晶振偏差 */

/*
 * PID 三个系数的量纲（都带时间，跟主循环快慢、跟 bInterval 都无关）：
 *     corr = Kp*err + Ki*∫err dt + Kd*d(err)/dt
 *   err  : 水位误差，单位 = 半字（1 个 32bit 样点 = 4 半字）
 *   corr : 速率修正，单位 = 帧/微帧
 *          （1 帧/微帧 = 8000 帧/s = 32000 半字/s，所以闭环时间常数 ≈ 1/(32000*Kp)）
 *   dt   : 定时器周期（SYNC_TICK_DT，常量），见下面的 TIM2 说明。
 *
 * 标称值本身不用调：Audio_UpdateNominal() 直接按 I2S 分频算真实采样率。
 */

/*
 * 反馈值的更新节拍：由 **硬件定时器 TIM2** 的更新中断驱动（见 Syscfg.c 的
 * AUDIO_SYNC_TIM_Init），不再依赖主循环里的 Delay_Ms —— 主循环会被 printf/串口
 * 拖慢到几十毫秒，根本不能当定时器用。中断频率就是 SYNC_TICK_HZ，
 * 所以 PID 的 dt 是常量，和主循环快慢、和 bInterval 都无关。
 *
 * 频率校验：打印行最后一列 = 本 tick 内收到多少个 USB 音频包。
 * bInterval=1 时每个微帧一包，1kHz tick 下应该稳定显示 8；
 * 如果显示 4，说明 TIM2 实际时钟是配置值的 2 倍（把 SYNC_TIMER_CLK_HZ 改成 144000000）。
 */
#define SYNC_TICK_HZ        1000u
#define SYNC_TICK_DT        (1.0f / (float)SYNC_TICK_HZ)

static volatile int32_t  s_level;      /* 最近一次水位（半字） */
static volatile uint32_t s_usb_mf;     /* 收到的 ISO 包数 ≈ 微帧数（时间基准，125us 一格）*/
static volatile uint32_t s_fb_fixed;   /* 当前反馈值（16.16），中断里直接发 */
static volatile uint32_t s_underrun;   /* 欠载次数 */
static uint32_t s_nominal_q16;         /* 标称值（16.16） */
static float    s_integral;            /* 积分项（单位：半字·秒） */
static float    s_err_last;            /* 上一次误差（D 项用） */
static float    s_corr_last;           /* 最近一次修正量（帧/微帧），只给打印看 */
static uint32_t s_tick_mf;             /* 上两个 tick 之间收到多少包（校验定时器频率） */
static uint32_t s_tick_mf_last;        /* 上次统计时的包数 */
static volatile uint32_t s_tick_ms;    /* TIM2 tick 计数（1kHz 时约等于毫秒） */
/* 中断里只置标志，真正的打印交给主循环 —— 中断里 printf 会把 USB 中断卡住 */
static volatile uint8_t  s_evt_open;
static volatile uint8_t  s_evt_close;
static volatile uint8_t  s_evt_fs;
static volatile uint32_t s_evt_fs_val;
static volatile uint32_t s_req_fs;     /* 非 0 = 主循环待处理的采样率变更请求 */
static volatile int32_t  s_lost;       /* 累计丢包数（期望 8 包/ms；上涨=真丢包） */
/*
 * 上溢（写指针撞读指针）保护触发次数。
 * 由 Audio_out_callback() 的逐帧保护累加：水位已经顶到"写指针要追上读指针"时，
 * 本包剩余数据直接丢弃并计数。正常情况下应该恒为 0；一旦上涨，说明主机灌得
 * 比 DMA 播得快（或水位目标设得过大），可从串口遥测行观察。
 */
static volatile uint32_t s_overflow;

static volatile uint32_t s_fs_hz = 48000u; /* 主机实际请求的采样率（GET_CUR 用它回答） */

static void Audio_UpdateNominal(void); /* 定义在后面，这里先声明 */
static void Audio_AlignWrite(void);    /* 定义在后面，这里先声明 */
static void Audio_FsChangePoll(void);  /* 定义在后面，这里先声明 */
static void CDC_TxPoll(void);          /* 定义在后面，这里先声明 */

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

static SAMPLE_RATE FS = FS_48000;

/*
 * 采样率表（Clock Source 的 Sampling Frequency Control 返回的 RANGE 结构）：
 *   [wNumSubRanges] 然后每个子范围 3 个 32bit：wMIN, wMAX, wRES
 * 注意 wRES 不能是 0 —— 规范里它是"分辨率"，必须 ≥1；写 0 有些主机会算不出来
 * 甚至解析错乱（手机尤其明显）。这里离散采样率，MIN==MAX，分辨率写 1 即可。
 */
static uint8_t g_audio_fs_table[] = {
    WBVAL((0x0002)),
    DBVAL(48000),DBVAL(48000),DBVAL(1),
    DBVAL(96000),DBVAL(96000),DBVAL(1),
};

static PID_struct PID = {
    .Set=0.0,
    .Actual=0.0,
    .err=0.0,
    .err_last=0.0,
    .voltage=0.0,
    .integral=0.0,
    .Kp=SYNC_KP_DEFAULT,   /* 直接在这里改默认增益，也可以用串口发 p=/i=/d= 在线调 */
    .Ki=SYNC_KI_DEFAULT,
    .Kd=0.0,
    .max_inte = 2000,
    .min_inte = -2000
};

bool g_update = false, //参数需要更新
     g_mute = false;   //静音
uint8_t g_volumn_db = 0;



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

/* ---------------------------------------------------------------------------
 * CDC 发送：**环形缓冲 + 主循环轮询**
 *
 * 为什么这么设计：
 *   usbd_ep_start_write() 只有在上一包发完（tx_busy==0）之后才能再调，
 *   而主机什么时候来取 CDC IN 的数据是不确定的（终端没打开时可能永远不来）。
 *   如果谁想打印就自己去等，那个上下文就被卡住 —— 如果那是 USB 中断，
 *   音频直接卡顿；如果中途超时放弃，一条日志只发出去一半，看起来就是乱码。
 *
 * 所以：打印 = 往环形缓冲里塞字节（永不阻塞、永不半条），
 *       实际发送由主循环 CDC_TxPoll() 做（每次发一包，发不出去就等下一轮）。
 * 生产者只有主循环一个（中断里一律只置标志，见 CDC_cmd_proc），
 * 所以 head/tail 不需要临界区。
 * ------------------------------------------------------------------------- */
#define CDC_TX_RING_SIZE  2048u                    /* 必须是 2 的幂 */
#define CDC_TX_RING_MASK  (CDC_TX_RING_SIZE - 1u)

static uint8_t  cdc_tx_ring[CDC_TX_RING_SIZE];
static volatile uint16_t cdc_tx_head;              /* 生产者 = 主循环 */
static volatile uint16_t cdc_tx_tail;              /* 消费者 = 主循环 */
static volatile uint32_t cdc_tx_drop;              /* 缓冲满而丢掉的字节数 */

/* 打印走的还是这个函数（_write 的落点），但已经改成"入队"，不再阻塞 */
void CDC_WriteBlocking(uint8_t* buf,int size){
    int i;

    if(size <= 0 || buf == NULL)
        return;

    for (i = 0; i < size; i++) {
        uint16_t next = (uint16_t)((cdc_tx_head + 1u) & CDC_TX_RING_MASK);

        if (next == cdc_tx_tail) {                 /* 满了：丢掉这一包剩下的 */
            cdc_tx_drop += (uint32_t)(size - i);
            break;
        }
        cdc_tx_ring[cdc_tx_head] = buf[i];
        cdc_tx_head = next;
    }
}

/* 主循环里调用：把环形缓冲里的数据发给 CDC，一包最多 sizeof(tx_buf) 字节 */
static void CDC_TxPoll(void){
    uint16_t n = 0;

    if (tx_busy) {
        return;                                    /* 上一包还没被主机取走 */
    }

    while ((n < (uint16_t)sizeof(tx_buf)) && (cdc_tx_tail != cdc_tx_head)) {
        tx_buf[n++] = cdc_tx_ring[cdc_tx_tail];
        cdc_tx_tail = (uint16_t)((cdc_tx_tail + 1u) & CDC_TX_RING_MASK);
    }
    if (n == 0u) {
        return;
    }

    tx_busy = 1;
    if (usbd_ep_start_write(0, EP_CDC_IN, tx_buf, n) != 0) {
        tx_busy = 0;                               /* 没发出去：把读指针退回去，下轮再发 */
        cdc_tx_tail = (uint16_t)((cdc_tx_tail - n) & CDC_TX_RING_MASK);
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
    (void)ep;
    /* 收完必须立刻续挂下一包：CDC OUT 收完一次就回到 NAK，不重新挂的话
     * 主机再写进来的数据永远收不到（串口工具会报"发不出去"）。
     * 收到的数据在 rx_buf 里，长度记下来给主循环解析用。 */
    usbd_ep_start_read(busid, EP_CDC_OUT, rx_buf, BUF_SIZE);
    g_cdc_out_len = (nbytes < BUF_SIZE) ? (uint16_t)nbytes : (uint16_t)(BUF_SIZE - 1);
    g_cdc_out = true;//置一，等待while处理
    return;
}

void CDC_cmd_proc(void){//这个函数留给while主循环调用
    /*
     * 主循环轮询要做的事（中断里只置标志，重活都在这儿做）：
     *   1) 把环形缓冲里的日志发给 CDC；
     *   2) 打印中断里记下的事件（采样率请求 / 开流 / 关流）；
     *   3) 采样率真的变了 -> 在这里做 I2S 重建（外设复位 + 对齐重启），
     *      这一步有毫秒级的等待，绝不能放在 USB 中断里做。
     */
    CDC_TxPoll();
    Audio_FsChangePoll();

    if (s_evt_fs) {
        s_evt_fs = 0;
        printf("FS_SET,%u\n", (unsigned)s_evt_fs_val);
    }
    if (s_evt_close) {
        s_evt_close = 0;
        printf("AS_CLOSE\n");
    }
    if (s_evt_open) {
        s_evt_open = 0;
        printf("AS_OPEN,%u\n", (unsigned)s_fs_hz);
    }

	if(g_cdc_out){
		g_cdc_out = false;

        /*
         * 注意：rx_buf 里是 USB 收到的裸数据，**不是以 0 结尾的字符串**！
         * 这里踩过的坑：
         *   1) 不补 '\0' 就 strstr()，会一直往后读到 BUF_SIZE 之外；
         *   2) 主机发的内容里没有 "p=" 时 strstr() 返回 NULL，紧接着 atof(NULL)
         *      直接 HardFault —— 而 HardFault_Handler 里是 NVIC_SystemReset()，
         *      于是设备**静默重启**：串口消失、上位机工具异常退出、再打开也打不开。
         * 所以：先按实际收到的长度补终止符，三个指针都拿到再解析；
         * 另外 atof 要从 "p=" 后面两位开始（"p=1.5" 直接 atof 出来是 0）。
         */
        rx_buf[g_cdc_out_len] = '\0';

        char *p_pt = strstr((char*)rx_buf,"p=");
        char *i_pt = strstr((char*)rx_buf,"i=");
        char *d_pt = strstr((char*)rx_buf,"d=");
        char *end = NULL;
        
        if(p_pt){
            float val = strtof(p_pt + 2,&end);
            PID_setp(&PID,val);
            printf("p=%f,i=%f,d=%f\n",val,PID.Ki,PID.Kd);
        }
        else if(i_pt){
            float val = strtof(i_pt + 2,&end);
            PID_seti(&PID,val);
            printf("p=%.5f,i=%.5f,d=%.5f\n",PID.Kp,val,PID.Kd);
        }
        else if(d_pt){
            float val = strtof(d_pt + 2,&end);
            PID_setd(&PID,val);
            printf("p=%.5f,i=%.5f,d=%.5f\n",PID.Kp,PID.Ki,val);
        }
	}
    /* 调试输出：不要每次主循环都打（1kHz 会把串口淹掉，也拖慢同步环）。
     * 节拍用 TIM2 的 tick 计数（不是收到的包数）—— 这样主机停流/暂停的时候
     * 打印照旧进行，才能看出"卡一下"到底持续了几行（= 多少毫秒）。 */
    {
        static uint32_t s_last_print_ms;

        if ((uint32_t)(s_tick_ms - s_last_print_ms) >= 50u) {
            int32_t  played_now, lvl_now;

            s_last_print_ms = s_tick_ms;

            /*
             * **在主循环里现算一遍水位**（用户要看实时值）：
             *   played = 缓冲长度 - DMA 剩余计数（DMA 一直在跑，随时可读）
             *   lvl    = (写指针 - played) mod 长度
             * 为什么要两个水位：
             *   第 1 列是 ISO 回调里量的（固定相位，PID 用它，稳）；
             *   最后一列是这里现量的 —— 微帧内水位本来是锯齿，所以它会在
             *   [目标 ± 一包的样点数] 之间跳，这是正常的，不是抖动。
             */
            played_now = (int32_t)((uint32_t)I2S_BUF_SIZE -
                                   (uint32_t)DMA_GetCurrDataCounter(DMA1_Channel5));
            lvl_now = (int32_t)i2s_write_idx - played_now;
            if (lvl_now < 0) {
                lvl_now += (int32_t)I2S_BUF_SIZE;
            } else if (lvl_now > (int32_t)I2S_BUF_SIZE) {
                lvl_now -= (int32_t)I2S_BUF_SIZE;
            }

            /*
             * 列：水位, 目标, 反馈值(16.16), 标称(16.16), 修正(1e-6 帧/微帧),
             *     误差(半字), Kp, Ki, 欠载次数, 本tick收到包数, **实时水位**
             * 看 PID 到底有没有在干活：第 3 列(反馈) 减第 4 列(标称) 就是 PID 的输出；
             * 第 10 列用来校验定时器频率：bInterval=1、tick=1kHz 时应该稳定是 8；
             * 第 11 列是主循环此刻现量的水位（锯齿是正常的）。
             */
            printf("%d,%d,%u,%u,%d,%d,%.5f,%.5f,%u,%u,%d,%d\n",
                   (int)s_level,                       /* 当前水位（半字） */
                   (int)s_target_level,                /* 目标水位 */
                   (unsigned)s_fb_fixed,               /* 反馈值 16.16 */
                   (unsigned)s_nominal_q16,            /* 标称值 16.16 */
                   (int)(s_corr_last * 1000000.0f),    /* PID 修正量 ×1e6 */
                   (int)((float)s_target_level - (float)s_level),  /* 误差 */
                   (double)PID.Kp, (double)PID.Ki,     /* 当前增益 */
                   (unsigned)s_underrun,               /* 欠载次数 */
                   (unsigned)s_tick_mf,                /* 本 tick 内的包数 */
                   (int)lvl_now,                       /* 主循环现量的实时水位 */
                   (int)s_lost);                       /* 累计丢包数 */
        }
    }
    return;

}

/* ---------------------------------------------------------------------------
 * I2S + DMA 的启停与写指针对齐
 *
 * 关键约定：**I2S + DMA 从开机起就一直跑，永远不停。**
 *   - 开机 Audio_Init() 把缓冲清 0 并启动它 → DAC 立刻有时钟、且是数字静音；
 *   - 之后无论是开音频、关音频、改采样率，都不去停它；
 *   - 因为读指针一直在走，任何时候都不能"从 0 开始写"，
 *     只能写在"当前播放位置 + 安全区"之后，而且要 4 半字（=1 帧）对齐。
 * ------------------------------------------------------------------------- */

/* DMA 已经播到哪（半字）：计数是"剩余"，所以位置 = 总长 - 剩余 */
static int32_t Audio_ReadPos(void)
{
    return (int32_t)((uint32_t)I2S_BUF_SIZE - (uint32_t)DMA_GetCurrDataCounter(DMA1_Channel5));
}

/*
 * 开机调用一次：缓冲清零 + 启动 I2S/DMA，此后永不停止。
 * 顺序要求：ES9018 先初始化好（main 里先 ES9018_Init），再让 I2S 出时钟。
 */
void Audio_Init(void)
{
    memset(i2s_tx_buf, 0, sizeof(i2s_tx_buf));
    i2s_write_idx = 0;
    i2s_read_idx  = 0;
    s_usb_mf = 0;
    s_tick_mf_last = 0;
    s_stream_idle = 0;
    s_idle_ticks = 0;
    Audio_UpdateNominal();          /* 顺便把目标水位/安全区按当前采样率算好 */

    I2S2_DMA_Init(i2s_tx_buf, I2S_BUF_SIZE);
    I2S2_DMA_Start(i2s_tx_buf, I2S_BUF_SIZE);
}

/*
 * 把写指针对到"读指针 + **目标水位**"之后（向上取整到 4 半字 = 1 帧），
 * 并把这段距离清成 0 —— 新数据到来之前 DAC 播的是静音，不会播到旧数据。
 *
 * 为什么对到"目标水位"而不是"安全区"：
 *   对到安全区（目标的一半）会让水位从半满开始，还得靠 PID 慢慢补上去，
 *   这段"补水位"的时间里余量只有一半，一旦主机送数稍慢就是一次欠载 ——
 *   也就是"切换设备后过几秒必然断一下"的那种。直接对到目标水位，
 *   切换/恢复之后立刻就在设定点上，余量也是满的，延迟不变（还是 4ms）。
 *
 * 打开音频流、欠载恢复、流停了又恢复时都会调用（不动 DMA，只挪写指针）。
 * 期间关 USB 中断：ISO 回调也在改这些量。
 */
static void Audio_AlignWrite(void)
{
    int32_t rp, end, k, dist;

    NVIC_DisableIRQ(USBHS_IRQn);

    /* 目标水位（保底不低于安全区），一帧对齐 */
    dist = s_target_level;
    if (dist < s_safe_margin) {
        dist = s_safe_margin;
    }
    dist = (dist + 3) & ~3;

    rp  = Audio_ReadPos();
    end = rp + dist;

    /* 这段距离填 0：4 半字一步（缓冲长度是 4 的倍数，所以 i..i+3 不会跨出数组） */
    for (k = (rp + 3) & ~3; k < end; k += 4) {
        int32_t i = k % (int32_t)I2S_BUF_SIZE;
        i2s_tx_buf[i]     = 0;
        i2s_tx_buf[i + 1] = 0;
        i2s_tx_buf[i + 2] = 0;
        i2s_tx_buf[i + 3] = 0;
    }

    i2s_write_idx = (int16_t)(((end + 3) & ~3) % (int32_t)I2S_BUF_SIZE);  /* 新数据从这里开始写 */
    i2s_read_idx  = (int16_t)(rp % (int32_t)I2S_BUF_SIZE);
    s_level    = dist;
    s_integral = 0.0f;
    s_err_last = 0.0f;

    NVIC_EnableIRQ(USBHS_IRQn);
}

/* ---------------------------------------------------------------------------
 * 同步环：由 **TIM2 更新中断**（1kHz，见 ch32v30x_it.c / Syscfg.c）调用。
 * 输出写进 s_fb_fixed，再由反馈端点中断 Audio_SendFeedback() 原样发出去。
 *
 * 为什么不用主循环：主循环里一旦有 printf/串口输出（CDC 没被读走时尤其明显），
 * 一次就能卡几十毫秒，节拍完全不准；定时器中断才是真正的 1kHz。
 * dt 直接取定时器周期（常量），所以 Kp/Ki/Kd 的量纲和主循环快慢无关。
 * ------------------------------------------------------------------------- */
void Audio_SyncTick(void)
{
    float err, derr, corr;

    /*
     * 注意：**这里绝对不能给 ES9018 复位**（之前试过，已删）。
     * 原因：I2S+DMA 从开机起一直在出 BCLK/LRCK，此时把 RST 拉低再放开，
     * 芯片的串行接收器会重新"找帧边界"，找到哪里是随机的 —— 于是表现为
     * 时好时坏：正常 / 左右声道互换 / 没声 / 满幅噪声，而且和 CPOL 无关。
     * 复位只能在**没有时钟**的时候做（即 ES9018_Init()，那时 I2S 还没启动）。
     */

    s_tick_ms++;                             /* 打印节拍用（主机暂停时也要继续打印） */
    s_tick_mf = s_usb_mf - s_tick_mf_last;   /* 本 tick 内收到的包数（打印用，校验定时器频率） */
    s_tick_mf_last = s_usb_mf;

    /*
     * 丢包统计：bInterval=1 时主机每毫秒正好发 8 个包（8kHz 微帧）。
     * 累加"本 tick 少收/多收几个" —— 正常抖动（这 ms 少 1 个、下 ms 多 1 个）
     * 会互相抵消，所以这个累计值上涨 = **真丢包**（同步传输丢了不会重传）。
     * 丢一包 = 少 6~13 个样点 = 一声轻微"咔"，而水位 50ms 才采一次，看不出来。
     * 主机停流（无音频会话）时不统计，否则会一路涨。
     */
    if (audio_open) {
        s_lost += 8 - (int32_t)s_tick_mf;
    }

    /*
     * 主机突然不送数据了 —— 电脑上"切换到别的输出设备"、或者任何 app/网页
     * 开始/停止用音频时，Windows 都会把流停掉再重开。此时 DMA 会继续读缓冲，
     * 但**只剩目标水位那点余量（4ms）**：超过 4ms 就开始重复播放旧数据，
     * 听起来就是"卡一下/一段怪声"。
     * 所以阈值取 **3ms**（比余量小，抢在重复旧数据之前把缓冲清成数字静音）；
     * 数据一旦回来，再把写指针重新对齐一次。I2S 时钟照旧不停。
     */
    if (s_tick_mf == 0u) {
        if (s_idle_ticks < 255u) {
            s_idle_ticks++;
        }
        if ((s_idle_ticks >= 3u) && (s_stream_idle == 0u)) {
            s_stream_idle = 1;
            memset(i2s_tx_buf, 0, sizeof(i2s_tx_buf));
        }
    } else {
        if (s_stream_idle != 0u) {
            s_stream_idle = 0;
            Audio_AlignWrite();          /* 数据回来了：写指针重新对齐到安全位置 */
        }
        s_idle_ticks = 0;
    }

    if (!audio_open) {
        return;
    }

    /*
     * 欠载（水位贴底）：播放位置已经越过写指针，DMA 在重复旧数据。
     * **不停 DMA**（时钟不能断），只把写指针挪到"当前播放位置 + 安全区"，
     * 并把中间清 0，让它先播一小段静音再恢复。
     */
    if (s_level <= 0) {
        s_underrun++;
        GPIO_ResetBits(GPIOC, GPIO_Pin_9); /* 原有欠载事件：异常灯锁存 */
        Audio_AlignWrite();
        return;
    }

    /*
     * PID：水位低了(err>0) -> 让主机送快点(corr>0)。符号不要弄反。
     *   err  = 目标水位 - 当前水位        （半字）
     *   corr = Kp*err + Ki*∫err dt + Kd*derr/dt   （帧/微帧）
     * dt = SYNC_TICK_DT（定时器周期，常量）
     */
    err = (float)s_target_level - (float)s_level;

    s_integral += err * SYNC_TICK_DT;
    if (s_integral >  SYNC_INTEG_LIMIT) s_integral =  SYNC_INTEG_LIMIT;
    if (s_integral < -SYNC_INTEG_LIMIT) s_integral = -SYNC_INTEG_LIMIT;

    derr = (err - s_err_last) / SYNC_TICK_DT;
    s_err_last = err;

    corr = (float)(PID.Kp * err + PID.Ki * s_integral + PID.Kd * derr);
    if (corr >  SYNC_CORR_LIMIT) corr =  SYNC_CORR_LIMIT;
    if (corr < -SYNC_CORR_LIMIT) corr = -SYNC_CORR_LIMIT;
    s_corr_last = corr;

    /* 反馈值 = 标称（真实采样率）+ 修正 */
    s_fb_fixed = s_nominal_q16 + (uint32_t)(int32_t)(corr * 65536.0f);
}

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

    /*
     * **不认识的采样率也要接受**（以前是直接 return，那会让主机以为切成功了、
     * 而 I2S 还在旧频率上跑 → 数据流和本机时钟对不上 → 一团噪声，
     * 手机插上来就是这个下场之一）。I2S 是整数分频，任何频率都能凑一个最接近的
     * 分频比；偏差由反馈+PID 去补，比"完全不理它"好得多。
     * 只挡明显不合理的值。
     */
    if ((sampling_freq < 8000u) || (sampling_freq > 192000u)) {
        return;
    }

    /*
     * FS 只是给"分频读不到时的兜底标称"和 GET_CUR 用的，取最接近的一档。
     * 本工程只对外声明 48k / 96k（24k 已移除），所以低于 48k 的请求按 48k 处理；
     * 真正的分频仍按主机请求的频率算（见 I2S_SetFs），不会因为这里归到 48k 就跑错。
     */
    if (sampling_freq <= 48000u) {
        FS = FS_48000;
    } else {
        FS = FS_96000;
    }
    s_fs_hz = sampling_freq;

    /*
     * 这里在 **USB 中断** 里，只记录请求；真正的 I2S 重建交给主循环
     * Audio_FsChangePoll() 做 —— 那里面有毫秒级等待（I2S2_DMA_Start 等第一个
     * 半字进 TX 缓冲、外设复位），放在中断里会把 USB 卡住。
     * 注意 s_fs_hz 必须在这里就更新：主机紧接着可能读 GET_CUR，回答必须一致。
     */
    s_req_fs = sampling_freq;
    s_evt_fs_val = sampling_freq;
    s_evt_fs = 1;

    return;
}

/*
 * 采样率变更的后续处理（**主循环**调用，不在中断里）：
 *   I2S_SetFs() 返回 0 = 分频没变 → 什么都不做，帧相位保持不动（关键）。
 *   返回 1 = 分频真的改了 → 只改 I2SPR 不够：SPI 里残留的 TX 缓冲/移位状态
 *   会让新流错开一个半字（"切换采样率后波形又乱"就是这个）。
 *   所以做**彻底重建**：I2S2_Reinit() 走外设复位，然后 DMA 与 I2S 一起、
 *   从缓冲第 0 个半字对齐启动。
 */
static void Audio_FsChangePoll(void)
{
    uint32_t freq = s_req_fs;

    if (freq == 0u) {
        return;
    }
    s_req_fs = 0;

    if (I2S_SetFs(freq) != 0u) {
        NVIC_DisableIRQ(TIM2_IRQn);
        I2S2_DMA_Stop();
        I2S2_Reinit(freq);                /* 外设复位 + 新分频，结束时 I2S 是关的 */
        memset(i2s_tx_buf, 0, sizeof(i2s_tx_buf));
        i2s_write_idx = 0;
        i2s_read_idx = 0;
        s_level = 0;
        s_integral = 0.0f;
        s_err_last = 0.0f;
        I2S2_DMA_Start(i2s_tx_buf, I2S_BUF_SIZE);   /* 对齐启动 */
        NVIC_EnableIRQ(TIM2_IRQn);
    }

    Audio_UpdateNominal();
    s_fb_fixed = s_nominal_q16;
    g_update = true;
}

uint32_t usbd_audio_get_sampling_freq(uint8_t busid, uint8_t ep)
{
    (void)busid;
    (void)ep;

    /* 回主机"我们现在实际跑多少"，而不是那个三档枚举 —— 手机可能请求 44.1k 之类，
     * 我们也照单接受（见 set_sampling_freq），回答必须一致，否则主机会来回切。 */
    return s_fs_hz;
}

void usbd_audio_get_sampling_freq_table(uint8_t busid, uint8_t ep, uint8_t **sampling_freq_table)
{   
    *sampling_freq_table = g_audio_fs_table;
}

/*
 * 反馈端点：UAC2 异步播放(AS 端点的 bmAttributes=0x05)时，主机靠这个端点
 * 送回来的速率值决定"下一个微帧发多少个样点"。**端点里必须有数据**，
 * 主机每次来取都要能取到；一直 NAK 的话主机拿不到速率，异步流就维持不住。
 *
 * 高速下格式是 16.16 定点，单位 = 每微帧样点数：
 *   48k 真实速率 48076.92 -> 6.009615 * 65536 = 393846
 *   96k 真实速率 96153.85 -> 12.01923 * 65536 = 787692
 * 这个值由 Audio_SyncProc()（主循环）算好放进 s_fb_fixed，这里只负责原样发出去 ——
 * 中断里不做浮点/PID，否则既慢又抖。
 */
static void Audio_SendFeedback(uint8_t busid){
    uint32_t fb = s_fb_fixed;

    audio_fb_buf[0] = (uint8_t)(fb);
    audio_fb_buf[1] = (uint8_t)(fb >> 8);
    audio_fb_buf[2] = (uint8_t)(fb >> 16);
    audio_fb_buf[3] = (uint8_t)(fb >> 24);

    (void)usbd_ep_start_write(busid, EP_AUDIO_FEEDBACK, audio_fb_buf, sizeof(audio_fb_buf));
}

/*
 * 重新算标称值：每微帧帧数（16.16 定点）。
 *   真实采样率 fS = 160MHz/(64*N)，每微帧帧数 = fS/8000 = 312.5/N，
 *   16.16 = 312.5*65536/N = 20480000/N —— 纯整数，避免浮点和取整误差。
 *   48k(N=52) -> 393846 = 6.009615 帧/微帧（主机就会按这个速率送数）。
 */
/*
 * 目标水位/安全区按"时间"折成半字数（采样率变了要跟着变，否则 96k 下余量只有一半）：
 *   每毫秒的数据量 = fS/1000 帧 = 该值 × 4 个半字
 *   48k -> 769/384 半字      96k -> 1538/769 半字
 * 上限是缓冲的一半（缓冲按 96k 8ms 开，见 Syscfg.h）。
 */
static void Audio_UpdateFsDependent(void)
{
    uint32_t fs = I2S_GetRealFs();
    uint32_t per_ms;

    if (fs == 0u) {
        fs = (uint32_t)FS;
    }
    per_ms = fs * 4u / 1000u;                        /* 每毫秒多少半字 */

    s_target_level = (int32_t)(per_ms * SYNC_TARGET_MS);
    s_safe_margin  = (int32_t)(per_ms * SYNC_MARGIN_MS);

    if (s_target_level > (int32_t)(I2S_BUF_SIZE / 2u)) {
        s_target_level = (int32_t)(I2S_BUF_SIZE / 2u);
    }
    if (s_safe_margin >= s_target_level) {
        s_safe_margin = s_target_level / 2;
    }
    s_safe_margin &= ~3;                             /* 4 半字 = 1 帧对齐 */
}

static void Audio_UpdateNominal(void)
{
    uint16_t n = I2S_GetDivN();

    if (n != 0u) {
        s_nominal_q16 = 20480000u / (uint32_t)n;
    } else {
        /* 分频寄存器读不到就退回整数标称，至少不至于完全错 */
        s_nominal_q16 = (uint32_t)(((uint32_t)FS / 8000u) << 16);
    }

    Audio_UpdateFsDependent();      /* 目标水位/安全区跟着采样率走 */
}

void usbd_audio_open(uint8_t busid, uint8_t intf)
{
    if(intf == AUDIO_AS_INTERFACE){
        s_usb_mf = 0;
        s_tick_mf_last = 0;
        s_tick_mf = 0;
        s_underrun = 0;
        s_lost = 0;
        Audio_UpdateNominal();
        s_fb_fixed = s_nominal_q16;   /* 第一包先给标称值，主机一取就有 */

        /*
         * I2S+DMA 从开机起一直在跑，所以**不能从 0 开始写**：
         * 把写指针对到"当前播放位置 + 安全区"之后（帧对齐），安全区填 0，
         * 新数据就从那里开始；PID 随后会把水位拉到目标值。
         */
        Audio_AlignWrite();

        usbd_ep_start_read(busid,EP_AUDIO_OUT,current_USB_audio_arr,AUDIO_RX_SIZE);
        audio_open = true;
        GPIO_ResetBits(GPIOC, GPIO_Pin_8); /* 低电平：音频打开 */
        Audio_SendFeedback(busid);   /* 反馈端点先挂上第一包 */

        /* 这里**不要**复位 ES9018：I2S 时钟一直在跑，复位会让芯片重新找帧边界，
         * 结果就是左右声道随机互换 / 没声 / 噪声（见 Audio_SyncTick 里的说明）。 */

        /* 诊断：Windows 在 app/网页/挂件开始用音频时会重开这条流，这里就会走到。
         * **只置标志**：本函数跑在 USB 中断里，printf 会把 USB 中断卡住（音频卡顿）。 */
        s_evt_open = 1;
    }
}

void usbd_audio_close(uint8_t busid, uint8_t intf)
{
    (void)busid;
    (void)intf;
        if(intf == AUDIO_AS_INTERFACE){
        current_USB_audio_arr = (uint8_t*)USB_audio_buf[0];

        /*
         * 关闭音频流：**不停 I2S/DMA**（时钟不能断），只停止写新数据。
         * 缓冲清成 0，这样 DMA 继续播的就是数字静音，而不是把最后 8ms 音频
         * 反复循环（那会听到嗡嗡声）。下次开流时 Audio_AlignWrite() 会把写指针
         * 重新对到播放位置之后。
         */
        audio_open = false;
        //memset(i2s_tx_buf, 0, sizeof(i2s_tx_buf));
        GPIO_SetBits(GPIOC, GPIO_Pin_8); /* 音频关闭：灭 */
        s_evt_close = 1;                    /* 诊断：主机主动关流（打印在主循环） */
    }
    
}


void Audio_out_callback(uint8_t busid, uint8_t ep, uint32_t nbytes){
    uint16_t* src_pt = (uint16_t*)current_USB_audio_arr,
            * dest_pt = (uint16_t*)i2s_tx_buf;
    uint16_t played;
    int32_t  lvl;

    current_USB_audio_arr = (current_USB_audio_arr == (uint8_t*)USB_audio_buf[0])?USB_audio_buf[1]:USB_audio_buf[0];
    usbd_ep_start_read(busid,EP_AUDIO_OUT,current_USB_audio_arr,AUDIO_RX_SIZE);

    /*
     * ===================== 逐帧上溢保护（写指针不得越过读指针）=====================
     *
     * 这是唯一会把**满幅噪声**送进耳机的事故：
     *   写指针一旦绕过读指针，水位 lvl 的环形运算会把"很满"读成"很小"，
     *   PI 反而让主机送得更快（反向死锁，永远回不来）；同时 DMA 播的是被
     *   覆盖到一半的数据，波形全乱。突发情况下（主机猛灌一大包）就可能发生。
     *
     * 做法：**每写一帧（= 2 个 32bit 样点 = 4 个半字）之前**先算
     *   "从写指针到读指针还有多少空位（环形）"，不够就停手、把本包剩下的丢掉。
     * 正常水位下空位有一半以上，这段判断永远不成立。
     * 丢掉的包计入 s_overflow，可用串口遥测行的"深水位保护次数"观察。
     *
     * 下溢方向（水位见底）由 Audio_SyncTick() 的欠载分支负责：
     * 立即重对齐并把播放位置之后填 0，保证 DMA 播到的是数字静音。
     */
    for(int i = 0;i<nbytes/2;i+=2){
        int32_t space = (int32_t)i2s_read_idx - (int32_t)i2s_write_idx;

        if (space <= 0) {
            space += I2S_BUF_SIZE;          /* 环形回绕 */
        }
        if (space < 4) {                    /* 一帧 = 4 半字，放不下就丢弃 */
            s_overflow++;
            GPIO_ResetBits(GPIOC, GPIO_Pin_9); /* 原有上溢事件：异常灯锁存 */
            break;
        }

        /* 搬数据：**这个顺序就是标准，不要再动**（用户实测确认）。
         *   缓冲里 = [样点高16位][样点低16位]  →  dest[0]=src[1], dest[1]=src[0]
         * USB 送来的 32bit 样点是小端（内存里先低后高），而 I2S 是 MSB first，
         * 所以这里交换一次。顺序反了 = 把样点低位当高位 = 满幅噪声（踩过）。 */
        dest_pt[i2s_write_idx+1] = src_pt[i];
        dest_pt[i2s_write_idx] = src_pt[i+1];
        i2s_write_idx = (2  + i2s_write_idx)%I2S_BUF_SIZE;
    }

    /*
     * 量水位 + 记时间基准 —— **必须在这里量**：
     * USB 数据是每微帧一次性写进来的（脉冲），I2S DMA 是连续播的，
     * 所以瞬时水位是锯齿；在主循环里随手量会量到锯齿的不同相位，PID 会发疯。
     * 这里每微帧量一次、相位固定，得到的才是"稳定水位"。
     * DMA 一直在跑，所以播到哪随时可读。
     */
    played = (uint16_t)(I2S_BUF_SIZE - (uint16_t)DMA_GetCurrDataCounter(DMA1_Channel5));
    i2s_read_idx = (int16_t)played;

    /* 环形水位：写成 (write - played) 而不是绝对值，绝对值在过零点会翻符号 */
    lvl = (int32_t)i2s_write_idx - (int32_t)played;
    if (lvl < 0) {
        lvl += (int32_t)I2S_BUF_SIZE;
    } else if (lvl > (int32_t)I2S_BUF_SIZE) {
        lvl -= (int32_t)I2S_BUF_SIZE;
    }
    s_level = lvl;
    s_usb_mf++;

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
        GPIO_SetBits(GPIOC, GPIO_Pin_8); /* USB 复位：播放灯灭 */
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

