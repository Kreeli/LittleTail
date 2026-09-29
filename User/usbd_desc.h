#pragma once
#include "debug.h"
#include "usbd_core.h"
#include "usbd_cdc.h"
#include "usbd_audio.h"
#define EP_CDC_INT 0x81
#define EP_CDC_IN 0x82
#define EP_CDC_OUT 0x02
#define EP_AUDIO_OUT 0x03
#define EP_AUDIO_FEEDBACK 0x83

#define CDC_MAXSIZE 512
#define AUDIO_MAXSIZE 512

#define CONFIG_DESC_LEN  (9 + CDC_ACM_DESCRIPTOR_LEN + AUDIO_AC_DESCRIPTOR_LEN(1) + 9 + 12 + 10 + AUDIO_AS_FEEDBACK_DESCRIPTOR_LEN(4))

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
    AUDIO_AC_DESCRIPTOR_INIT(
        0x02,//接口号0x02
        0x02,//两个interface,control和DATA
        9+12+9 + 10,///从header到终端的长度
        0x03,//字符串
        0x03//bainterfaceNr 每个音频流interface从哪里开始 第一个音频interface是0x03,这里可以填多个*/
    ),
    //输入终端
    AUDIO_AC_INPUT_TERMINAL_DESCRIPTOR_INIT(
        0x01,//AS ID
        0x0101,//USB stream是输入终端
        0x02,//双声道
        0x0003//左右立体声
    ),
    AUDIO_AC_FEATURE_UNIT_DESCRIPTOR_INIT(
        0x03,                   // bUnitID
        0x01,                   // bSourceID = IT
        0x01,                   // bControlSize
        0x00,                   // bmaControls(0) master
        0x03,                   // ch1: mute+volume
        0x03                    // ch2: mute+volume
    ),
    //输出终端
    AUDIO_AC_OUTPUT_TERMINAL_DESCRIPTOR_INIT(
        0x02,//AS ID
        0x0301,//Speaker是输出终端
        0x01//数据源的ID是0x01
    ),
    //音频流描述，一大坨，用一个宏解决了
    AUDIO_AS_FEEDBACK_DESCRIPTOR_INIT(
        0x03,//第0x03个接口
        0x02,//绑定output terminal
        0x02,//双声道
        0x04,//4 bytes per frame
        32,//32位有效位数
        EP_AUDIO_OUT,//端点地址
        AUDIO_MAXSIZE,//最大包长度
        0x01,//binteval在同步传输端点必须1
        EP_AUDIO_FEEDBACK,//反馈端点地址
        //采样率
        ((12000) & 0xFF), (((12000) >> 8) & 0xFF), (((12000) >> 16) & 0xFF),
        ((24000) & 0xFF), (((24000) >> 8) & 0xFF), (((24000) >> 16) & 0xFF),
        ((48000) & 0xFF), (((48000) >> 8) & 0xFF), (((48000) >> 16) & 0xFF),
        ((96000) & 0xFF), (((96000) >> 8) & 0xFF), (((96000) >> 16) & 0xFF)
    )
};
