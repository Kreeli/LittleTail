#pragma once
#include "debug.h"
#include "stdbool.h"
typedef enum{
    FS_24000 = 24000,
    FS_48000 = 48000,
    FS_96000 = 96000
}SAMPLE_RATE;


const uint8_t *get_dev(uint8_t speed);
const uint8_t *get_cfg(uint8_t speed);
const char *get_str(uint8_t speed, uint8_t index);

void USB_init(void);
void USBHS_RCC_init(void);
/* 音频同步环：由 TIM2 更新中断（1kHz）调用，不在主循环里跑。
 * 输出写进反馈值，由反馈端点中断发出去；主循环只负责打印状态。 */
void Audio_SyncTick(void);
/* 开机调用一次：缓冲清 0 并启动 I2S/DMA —— 之后永不停止（DAC 始终有时钟） */
void Audio_Init(void);
void CDC_WriteBlocking(uint8_t* buf,int size);
void CDC_Notified_callback(uint8_t busid, uint8_t ep, uint32_t nbytes);
void CDC_in_callback(uint8_t busid, uint8_t ep, uint32_t nbytes);
void CDC_out_callback(uint8_t busid, uint8_t ep, uint32_t nbytes);
void CDC_cmd_proc(void);
void Audio_out_callback(uint8_t busid, uint8_t ep, uint32_t nbytes);

void Audio_feedback_callback(uint8_t busid, uint8_t ep, uint32_t nbytes);

void usb_event_handler(uint8_t busid, uint8_t event);