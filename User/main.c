#include "debug.h"
#include "Syscfg.h"
#include "ES9018.h"
#include "usb_app.h"
#include "math.h"
#include "pid.h"
/*
 * I2S2 主机 32bit 发送
 * SPI DATAR 16bit，每个样点拆成 [高16][低16]，MSB first。
 * 立体声交错：[L0_hi][L0_lo][R0_hi][R0_lo]...
 */

extern bool g_cdc_out;
extern PID_struct PID;
void cdc_cmd_proc(void);
int main(void)
{
	NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);
	SystemCoreClockUpdate();
	Delay_Init();

	/*
	 * 必须关掉 stdout 缓冲！
	 * 本工程用 --specs=nano.specs --specs=nosys.specs，_fstat/_isatty 是空桩，
	 * newlib 就认为 stdout 不是字符设备 -> **全缓冲（BUFSIZ=1024）**：
	 * printf 的内容先进 1KB 缓冲，攒不满 1KB 就一个字节都不会走 _write 到 CDC。
	 * 你现在 100ms 打一行约 12 字节，要 85 行（≈8.5 秒）才突然吐一次。
	 * 关成无缓冲后，每行 printf 立刻发出去。
	 */
	setvbuf(stdout, NULL, _IONBF, 0);

	Syscfg();
	Delay_Ms(100);
	
	USB_init();
	ES9018_Init();
	/*
	 * I2S + DMA 从这里就启动，**之后永远不关闭**：
	 *   缓冲先清零 → 开机后 DAC 一直收到有效时钟 + 数字静音；
	 *   打开音频流时由 Audio_AlignWrite() 把写指针对到"播放位置 + 安全区"之后开始写。
	 */
	I2S2_ClockInit();
	I2S2_Init();
	Audio_Init();

	while (1) {
		CDC_cmd_proc();       /* 串口指令 + 同步状态打印 */
		Delay_Ms(1);
		/* 注意：音频同步环**不在**主循环里 —— 它由 TIM2 更新中断(1kHz)驱动：
		 *   ch32v30x_it.c 的 TIM2_IRQHandler() -> Audio_SyncTick()
		 * 主循环的 Delay_Ms / printf 再慢也不影响反馈更新节拍。 */
	}
}

