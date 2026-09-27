#pragma once
#include "debug.h"
#include "usbd_core.h"
#include "usbd_cdc.h"

#define EP_CDC_INT 0x81
#define EP_CDC_IN 0x82
#define EP_CDC_OUT 0x02
#define CDC_MAXSIZE 512

#define CONFIG_DESC_LEN  (9 + CDC_ACM_DESCRIPTOR_LEN)

uint8_t device_desc[] = {
    USB_DEVICE_DESCRIPTOR_INIT(
        USB_2_0,
        0xEF, 0x02, 0x01,  // CDC功能加Audio功能
        0x1209, 0x000,    // VID / PID（先用 WCH 的）
        0x0100,            // bcdDevice
        0x01               // bNumConfigurations
    )
};

uint8_t conf_desc[] = {
    USB_CONFIG_DESCRIPTOR_INIT(
        CONFIG_DESC_LEN,
        0x02,//接口数量
        0x01,//表示起始，没看懂
        USB_CONFIG_BUS_POWERED,//总线供电
        0xFA//最大供电
    ),
    CDC_ACM_DESCRIPTOR_INIT(
        0x00,
        EP_CDC_INT,
        EP_CDC_OUT,
        EP_CDC_IN,
        CDC_MAXSIZE,
        0x00
    )
};

