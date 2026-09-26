/********************************** (C) COPYRIGHT *******************************
* File Name          : main.c
* Author             : WCH
* Version            : V1.0.0
* Date               : 2021/06/06
* Description        : Main program body.
*********************************************************************************
* Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/

#include "debug.h"
#include "Syscfg.h"
#include "ES9018.h"

/*
 * I2S 32bit 发送（SPI DATAR 16bit，每个样点拆 2 个半字）
 *
 * 官方 HostRx_SlaveTx 32bit 顺序：
 *   SendData(sample>>16);  // 先高 16，含 MSB/bit31
 *   SendData(sample);      // 再低 16
 *
 * DMA 缓冲按此顺序：[hi][lo][hi][lo]...
 * 立体声：[L_hi][L_lo][R_hi][R_lo]...
 *
 * 不能把 uint32_t 数组直接给 HalfWord DMA：
 * 小端 0x12345678 在内存是 [0x5678,0x1234]，会先发低字。
 */

/* 若波形仍乱，改成 1 对调高低半字 */
#define I2S_LOW_HALF_FIRST  1

/* 1 = 只发 0x12345678，方便示波器核对位序 */
#define I2S_TEST_PATTERN    0

#define SINE_PERIOD     48u
#define SINE_FRAMES     (SINE_PERIOD * 4u)
#define SINE_TX_LEN     (SINE_FRAMES * 2u * 2u)

static uint16_t i2s_tx_buf[SINE_TX_LEN];

static const int32_t sine_quarter[13] = {
    0x00000000, 0x0C87CFCC, 0x18D8BCA2, 0x24BCD3FA,
    0x30000000, 0x3A70EBBF, 0x43E1DB33, 0x4C2973A2,
    0x532370B9, 0x58B1436E, 0x5CBA97D6, 0x5F2DBFB9,
    0x60000000
};

static void Sine_GenTable(int32_t *tab, uint32_t n)
{
    uint32_t q = n / 4u;
    uint32_t i;

    for (i = 0; i < n; i++) {
        uint32_t pos = i % q;
        if (i < q) {
            tab[i] = sine_quarter[pos];
        } else if (i < 2u * q) {
            tab[i] = sine_quarter[q - pos];
        } else if (i < 3u * q) {
            tab[i] = -sine_quarter[pos];
        } else {
            tab[i] = -sine_quarter[q - pos];
        }
    }
}

static void Sine_PutSample(uint16_t *dst, uint32_t idx, int32_t sample)
{
    uint32_t u = (uint32_t)sample;
    uint16_t hi = (uint16_t)(u >> 16);
    uint16_t lo = (uint16_t)(u & 0xFFFFu);

#if I2S_LOW_HALF_FIRST
    dst[idx * 2u]     = lo;
    dst[idx * 2u + 1] = hi;
#else
    dst[idx * 2u]     = hi;
    dst[idx * 2u + 1] = lo;
#endif
}

static void Sine_BuildTx(uint16_t *tx, uint32_t frames)
{
    int32_t tab[SINE_PERIOD];
    uint32_t i;

#if I2S_TEST_PATTERN
    /* 0x20000000 ≈ -6dBFS，常数 → 模拟端应是平直 DC */
    for (i = 0; i < frames * 2u; i++) {
        Sine_PutSample(tx, i, (int32_t)0x20000000);
    }
    return;
#endif

    Sine_GenTable(tab, SINE_PERIOD);

    for (i = 0; i < frames; i++) {
        int32_t s = tab[i % SINE_PERIOD];
        Sine_PutSample(tx, i * 2u, s);
        Sine_PutSample(tx, i * 2u + 1u, s);
    }
}

int main(void)
{
	NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);
	SystemCoreClockUpdate();
	Delay_Init();
	Syscfg();
	Delay_Ms(100);
	ES9018_Init();


	Sine_BuildTx(i2s_tx_buf, SINE_FRAMES);
	I2S2_DMA_Init(i2s_tx_buf, SINE_TX_LEN);
	I2S2_DMA_Start();

	while(1)
	{
	}
}