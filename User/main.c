#include "debug.h"
#include "Syscfg.h"
#include "ES9018.h"
#include "usb_app.h"
#include "math.h"
/*
 * I2S2 主机 32bit 发送
 * SPI DATAR 16bit，每个样点拆成 [高16][低16]，MSB first。
 * 立体声交错：[L0_hi][L0_lo][R0_hi][R0_lo]...
 */

// #define FRAME_NUM     	96*4u//I2S缓冲区存放的数据量 96k采样率下，会有2ms延迟
// #define I2S_BUF_SIZE     (FRAME_NUM * 2u * 2u)

// static uint16_t i2s_tx_buf[I2S_BUF_SIZE];
// extern bool g_update,g_mute;//静音
// extern uint8_t g_volumn_db;
// extern SAMPLE_RATE FS;

// static void Sine_BuildTx(void)
// {
// 	uint32_t i;

// 	for (i = 0; i < FRAME_NUM; i++) {
// 		int32_t u = (int32_t)(sin(M_PI * 2 / FRAME_NUM * i) * 0x7fffffff);
// 		uint16_t hi = (uint16_t)(u >> 16);
// 		uint16_t lo = (uint16_t)(u & 0xFFFFu);

// 		i2s_tx_buf[i * 4u + 0u] = hi;  /* L 高 16 */
// 		i2s_tx_buf[i * 4u + 1u] = lo;  /* L 低 16 */
// 		i2s_tx_buf[i * 4u + 2u] = hi;  /* R 高 16 */
// 		i2s_tx_buf[i * 4u + 3u] = lo;  /* R 低 16 */
// 	}
// }

int main(void)
{
	NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);
	SystemCoreClockUpdate();
	Delay_Init();
	Syscfg();
	Delay_Ms(100);
	
	USB_init();
	ES9018_Init();
	
	
	while (1) {
	}
}
