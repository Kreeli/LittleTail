#include "debug.h"
#include "Syscfg.h"
#include "ES9018.h"

/*
 * I2S2 主机 16bit 发送
 * 每个样点就是一个 uint16/int16，L,R 交错：
 *   [L][R][L][R]...
 */

#define SINE_PERIOD     48u
#define SINE_FRAMES     (SINE_PERIOD * 4u)
#define SINE_TX_LEN     (SINE_FRAMES * 2u)

static uint16_t i2s_tx_buf[SINE_TX_LEN];

static const int16_t sine_tab[SINE_PERIOD] = {
    0x0000, 0x0C87, 0x18D8, 0x24BC,
    0x3000, 0x3A70, 0x43E1, 0x4C29,
    0x5323, 0x58B1, 0x5CBA, 0x5F2D,
    0x6000, 0x5F2D, 0x5CBA, 0x58B1,
    0x5323, 0x4C29, 0x43E1, 0x3A70,
    0x3000, 0x24BC, 0x18D8, 0x0C87,
    0x0000, 0xF378, 0xE727, 0xDB43,
    0xD000, 0xC58F, 0xBC1E, 0xB3D6,
    0xACDC, 0xA74E, 0xA345, 0xA0D2,
    0xA000, 0xA0D2, 0xA345, 0xA74E,
    0xACDC, 0xB3D6, 0xBC1E, 0xC58F,
    0xD000, 0xDB43, 0xE727, 0xF378
};

static void Sine_BuildTx(void)
{
	uint32_t i;

	for (i = 0; i < SINE_FRAMES; i++) {
		int16_t s = sine_tab[i % SINE_PERIOD];
		i2s_tx_buf[i * 2u]     = (uint16_t)s;  /* L */
		i2s_tx_buf[i * 2u + 1] = (uint16_t)s;  /* R */
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

	Sine_BuildTx();
	I2S2_DMA_Init(i2s_tx_buf, SINE_TX_LEN);
	I2S2_DMA_Start();

	while (1) {
	}
}
