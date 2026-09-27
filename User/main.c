#include "debug.h"
#include "Syscfg.h"
#include "ES9018.h"
#include "usb_app.h"

/*
 * I2S2 主机 32bit 发送
 * SPI DATAR 16bit，每个样点拆成 [高16][低16]，MSB first。
 * 立体声交错：[L0_hi][L0_lo][R0_hi][R0_lo]...
 */

#define SINE_PERIOD     48u
#define SINE_FRAMES     (SINE_PERIOD * 4u)
#define SINE_TX_LEN     (SINE_FRAMES * 2u * 2u)

static uint16_t i2s_tx_buf[SINE_TX_LEN];

static const int32_t sine_tab[SINE_PERIOD] = {
    0x00000000, 0x0C87CFCC, 0x18D8BCA2, 0x24BCD3FA,
    0x30000000, 0x3A70EBBF, 0x43E1DB33, 0x4C2973A2,
    0x532370B9, 0x58B1436E, 0x5CBA97D6, 0x5F2DBFB9,
    0x60000000, 0x5F2DBFB9, 0x5CBA97D6, 0x58B1436E,
    0x532370B9, 0x4C2973A2, 0x43E1DB33, 0x3A70EBBF,
    0x30000000, 0x24BCD3FA, 0x18D8BCA2, 0x0C87CFCC,
    0x00000000, 0xF3783034, 0xE727435E, 0xDB432C06,
    0xD0000000, 0xC58F1441, 0xBC1E24CD, 0xB3D68C5E,
    0xACDC8F47, 0xA74EBC92, 0xA345682A, 0xA0D24047,
    0xA0000000, 0xA0D24047, 0xA345682A, 0xA74EBC92,
    0xACDC8F47, 0xB3D68C5E, 0xBC1E24CD, 0xC58F1441,
    0xD0000000, 0xDB432C06, 0xE727435E, 0xF3783034
};

static void Sine_BuildTx(void)
{
	uint32_t i;

	for (i = 0; i < SINE_FRAMES; i++) {
		uint32_t u = (uint32_t)sine_tab[i % SINE_PERIOD];
		uint16_t hi = (uint16_t)(u >> 16);
		uint16_t lo = (uint16_t)(u & 0xFFFFu);

		i2s_tx_buf[i * 4u + 0u] = hi;  /* L 高 16 */
		i2s_tx_buf[i * 4u + 1u] = lo;  /* L 低 16 */
		i2s_tx_buf[i * 4u + 2u] = hi;  /* R 高 16 */
		i2s_tx_buf[i * 4u + 3u] = lo;  /* R 低 16 */
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
	ES9018_SetVolume(0);
	USB_CDC_init();

	while (1) {
		printf("hello\n");
	}
}
