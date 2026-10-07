#pragma once

#include "debug.h"
void Syscfg(void);
void LED_Init(void);
void I2C2_init(void);
void ES9018_RST_Init(void);
void HP_AMP_RST_Init(void);
void TIM1_Init();

void I2S2_ClockInit();
void I2S2_Init(void);
/* 设置 I2S 采样率。返回 1 = 分频真的改了（I2S 帧相位被打断，调用方必须重建流）；
 * 返回 0 = 分频没变，什么都没做（主机重复发 SET_CUR 时靠这个保住帧相位）。
 * 别把它改回 void —— 见 Syscfg.c 里的详细说明。 */
uint8_t I2S_SetFs(uint32_t freq);
void I2S2_DMA_Init(const uint16_t *buf, uint16_t halfword_count);
void I2S2_DMA_Start(const uint16_t *buf, uint16_t halfword_count);
/* 彻底重建 I2S 外设（含外设复位 SPI_I2S_DeInit）并把分频设为 freq；
 * 结束时 I2S 是关着的，交给 I2S2_DMA_Start() 在"第一个半字已进 TX 缓冲"之后再使能。
 * 采样率切换后必须用它 —— 只改 I2SPR 会因为 SPI 里残留的半个字导致波形错乱。 */
void I2S2_Reinit(uint32_t freq);
void I2S2_DMA_Stop(void);
void I2S2_DMA_Recover(void);
/* I2S2_Underrun() 已删除：从未被调用过。曾用它查"爆米花"是否为 I2S 发送下溢
 * （UDR），实测从未触发 —— 真凶是 ES9018 的 Reg0x0A stop_div 被写坏，见 ES9018.c。 */

/* 音频同步环的节拍定时器（TIM2 @1kHz），在 Syscfg() 里初始化；
 * 更新中断里调用 Audio_SyncTick()（见 ch32v30x_it.c） */
void AUDIO_SYNC_TIM_Init(void);

/* I2S 分频的真实值：N = 2*I2SDIV + ODD，fS_real = 160MHz / (64*N)
 *   48k -> N=52 -> 48076.92Hz      96k -> N=26 -> 96153.85Hz
 * 音频同步的"标称反馈值"必须用这个真实速率，不能用整数 48000/96000。 */
uint16_t I2S_GetDivN(void);
uint32_t I2S_GetRealFs(void);

/* I2S 播放缓冲（单位：半字 = int16_t 个数 = 4 个半字/样点帧）。
 * 按**最坏情况（96k）**开 8ms：96k -> 8ms，48k -> 16ms，24k -> 32ms。
 * 实际用的水位不取缓冲一半，而是按时间算的 4ms（见 usb_app.c 的
 * Audio_UpdateFsDependent()）：48k 768 半字、96k 1538 半字，
 * 这样两种采样率的时间余量一致，96k 不会因为余量只有一半而频繁欠载。 */
#define I2S_BUF_HALFWORDS   3072u
