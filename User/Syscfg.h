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
void I2S_SetFs(uint32_t freq);
void I2S2_DMA_Init(const uint16_t *buf, uint16_t halfword_count);
void I2S2_DMA_Start(void);
void I2S2_DMA_Stop(void);
void I2S2_DMA_Recover(void);
uint8_t I2S2_Underrun(void);
