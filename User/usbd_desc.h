#pragma once
#include "debug.h"
#include "usbd_core.h"
#include "usbd_cdc.h"
#include "usbd_audio.h"
#define EP_CDC_INT 0x81
#define EP_CDC_IN 0x82
#define EP_CDC_OUT 0x02
#define EP_AUDIO_OUT 0x03
#define EP_AUDIO_FEEDBACK 0x84

#define BINTERVAL 0x01

#define AUDIO_AC_INTERFACE 0X02
#define AUDIO_AS_INTERFACE 0x03

#define CDC_MAXSIZE 512
#define AUDIO_MAXSIZE 1024

#define AUDIO_FU_LEN        (6 + ((2) + 1) * 4) /* 6+3*4 = 18 */
#define AUDIO_AC_ENTITY_LEN (8 + 17 + AUDIO_FU_LEN + 12)     /* Clock+IT+FU+OT = 55 */
#define AUDIO_AC_WTOTALLEN  (9 + AUDIO_AC_ENTITY_LEN)        /* Header+实体 = 64 */
#define CONFIG_DESC_LEN     (9 + CDC_ACM_DESCRIPTOR_LEN +                        \
                             AUDIO_V2_AC_DESCRIPTOR_LEN + AUDIO_AC_ENTITY_LEN +  \
                             AUDIO_V2_AS_FEEDBACK_DESCRIPTOR_LEN) /* 218 */

uint8_t device_desc[] = {
    USB_DEVICE_DESCRIPTOR_INIT(
        USB_2_0,
        0xEF, 0x02, 0x01,  // CDC功能加Audio功能的复合设备
        0x1209, 0x0000,    // VID / PID
        0x0100,            // bcdDevice
        0x01               // bNumConfigurations
    )
};

uint8_t conf_desc[] = {
    /*USB配置描述符 9bit*/
    USB_CONFIG_DESCRIPTOR_INIT(
        CONFIG_DESC_LEN,
        0x04,//接口数量
        0x01,//表示起始，没看懂
        USB_CONFIG_BUS_POWERED,//总线供电
        0xFA//最大供电
    ),
    /*CDC的配置描述符调用的现成的 CDC_ACM_DESCRIPTOR_LEN*/
    CDC_ACM_DESCRIPTOR_INIT(
        0x00,
        EP_CDC_INT,
        EP_CDC_OUT,
        EP_CDC_IN,
        CDC_MAXSIZE,
        0x00
    ),
    //IAD + 控制interface的描述符 + Header
    /*
     * 第三个参数是 AC Header 的 wTotalLength：AC 接口自身class描述符的**总字节数**
     * （含本 header 9 字节 + 所有实体）。UAC2 主机就按它遍历实体，
     * 写小了它会在实体中间"截断"，后面的 Output Terminal 就找不到了。
     *
     * 本工程实际是：9(header) + 8(Clock) + 17(IT) + 18(FU) + 12(OT) = 64
     * 以前这里手写成 9+11+18+12 = 50，主机只解析到 Feature Unit 的第 16 字节为止，
     * Output Terminal 被吃掉 -> 音频功能描述符非法。
     * 所以这里必须用推导出来的 AUDIO_AC_WTOTALLEN，不要手写数字。
     */
    AUDIO_V2_AC_DESCRIPTOR_INIT(
        AUDIO_AC_INTERFACE,//接口号0x02
        0x02,//两个interface,control和DATA
        64,//从header到终端的长度 = 64
        0x01,//bcatagory
        0x00,//这个没看懂，什么非可寻址控制功能的操作类型
        0x03//字符串
    ),
    
    //时钟源终端
    AUDIO_V2_AC_CLOCK_SOURCE_DESCRIPTOR_INIT(
        0x01, /* bClockID */
        0x00, /* bmAttributes */
        0x03  /* bmControls */
    ),
    //输入终端
    AUDIO_V2_AC_INPUT_TERMINAL_DESCRIPTOR_INIT(
        0x02,//AS ID
        0x0101,//USB stream是输入终端
        0x01,//时钟源
        0x02,//双声道
        0x00000003,//左右立体声
        0x0000//bmcontrol
    ),
    //特性控制终端
    AUDIO_V2_AC_FEATURE_UNIT_DESCRIPTOR_INIT(
        0x03,//ID
        0x02,//source
        0x03, 0x00, 0x00, 0x00, /* master: Mute + Volume */
        0x00, 0x00, 0x00, 0x00, /* ch1 (L): 无 */
        0x00, 0x00, 0x00, 0x00  /* ch2 (R): 无 */
    ),
    //输出终端
    AUDIO_V2_AC_OUTPUT_TERMINAL_DESCRIPTOR_INIT(
        0x04,//AS ID
        0x0301,//USB stream是输入终端
        0x03,//source
        0x01,//时钟源
        0x0000//bmcontrol
    ),
    //音频流描述，一大坨，用一个宏解决了
    AUDIO_V2_AS_FEEDBACK_DESCRIPTOR_INIT(
        AUDIO_AS_INTERFACE,           /* bInterfaceNumber */
        0x02,           /* bTerminalLink = IT(USB Streaming)，不是 OT(4) */
        0x02, /* bNrChannels */
        0x00000003,     /* bmChannelConfig FL+FR */
        0x04,           /* bSubslotSize = 4 (32 bit) */
        32,             /* bBitResolution */
        EP_AUDIO_OUT,   /* 数据端点 */
        AUDIO_MAXSIZE,
        BINTERVAL,           /* bInterval：高速下每微帧 */
        EP_AUDIO_FEEDBACK
    )
};

/*
 * 兜底：数组真实字节数必须等于配置描述符里声明的 wTotalLength，
 * 并且 AC Header 的 wTotalLength 必须等于 AC 实体的实际长度。
 * 这两处不一致主机就直接解析失败，而且从源码上完全看不出来 —— 只有编译期断言能挡住。
 */
_Static_assert(sizeof(conf_desc) == CONFIG_DESC_LEN,
               "conf_desc size != CONFIG_DESC_LEN");
_Static_assert(AUDIO_AC_WTOTALLEN == 9 + 8 + 17 + AUDIO_FU_LEN + 12,
               "AUDIO_AC_WTOTALLEN does not match the AC entities");
