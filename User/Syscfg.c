#include "Syscfg.h"
void Syscfg(void){
	LED_Init();
	I2C2_init();
	ES9018_RST_Init();
	HP_AMP_RST_Init();
	I2S2_Init();
    TIM1_Init();
}

void LED_Init(){
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC, ENABLE);
	GPIO_InitTypeDef GPIO_Init_S = {
		.GPIO_Mode = GPIO_Mode_Out_PP,
		.GPIO_Speed = GPIO_Speed_2MHz,
		.GPIO_Pin = GPIO_Pin_7 | GPIO_Pin_8 | GPIO_Pin_9
	};
	GPIO_Init(GPIOC,&GPIO_Init_S);
	GPIO_ResetBits(GPIOC,GPIO_Pin_7 | GPIO_Pin_8 | GPIO_Pin_9);
}

void I2C2_init(){
	RCC_APB1PeriphClockCmd(RCC_APB1Periph_I2C2, ENABLE);
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);

	GPIO_InitTypeDef GPIO_Init_S = {
		.GPIO_Mode = GPIO_Mode_AF_OD,
		.GPIO_Speed = GPIO_Speed_10MHz,
		.GPIO_Pin = GPIO_Pin_10 | GPIO_Pin_11
	};
	GPIO_Init(GPIOB,&GPIO_Init_S);
	I2C_InitTypeDef I2C_init_S = {
		.I2C_Ack = I2C_Ack_Enable,
		.I2C_AcknowledgedAddress = I2C_AcknowledgedAddress_7bit,
		.I2C_OwnAddress1 = 0X77,
		.I2C_ClockSpeed = 80000,
		.I2C_DutyCycle = I2C_DutyCycle_2,
		.I2C_Mode = I2C_Mode_I2C
	};
	I2C_Init(I2C2,&I2C_init_S);
	I2C_Cmd(I2C2,ENABLE);
}

void ES9018_RST_Init(void){
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);
	GPIO_InitTypeDef GPIO_Init_S = {
		.GPIO_Mode = GPIO_Mode_Out_PP,
		.GPIO_Speed = GPIO_Speed_2MHz,
		.GPIO_Pin = GPIO_Pin_14
	};
	GPIO_Init(GPIOB, &GPIO_Init_S);
	GPIO_SetBits(GPIOB, GPIO_Pin_14);  // 默认高电平，不复位
}

void HP_AMP_RST_Init(void){
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
	GPIO_InitTypeDef GPIO_Init_S = {
		.GPIO_Mode = GPIO_Mode_Out_PP,
		.GPIO_Speed = GPIO_Speed_2MHz,
		.GPIO_Pin = GPIO_Pin_5
	};
	GPIO_Init(GPIOA, &GPIO_Init_S);
	GPIO_SetBits(GPIOA, GPIO_Pin_5);  // 默认高电平，不复位
}

void TIM1_Init()
{
    NVIC_InitTypeDef NVIC_InitStructure = {0};
    TIM_TimeBaseInitTypeDef TIM_TimeBaseInitStructure = {0};

    // 1. 使能定时器时钟
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_TIM1, ENABLE);
    
    // 2. 配置定时器基本参数
    TIM_TimeBaseInitStructure.TIM_Period = 5000 - 1;                    // 自动重装载值
    TIM_TimeBaseInitStructure.TIM_Prescaler = 14400 - 1;                 // 预分频系数
    TIM_TimeBaseInitStructure.TIM_ClockDivision = TIM_CKD_DIV1;   // 时钟分频
    TIM_TimeBaseInitStructure.TIM_CounterMode = TIM_CounterMode_Up; // 向上计数
    TIM_TimeBaseInitStructure.TIM_RepetitionCounter = 0;          // 重复计数(高级定时器)
    TIM_TimeBaseInit(TIM1, &TIM_TimeBaseInitStructure);

    // 3. 清除更新中断标志
    TIM_ClearITPendingBit(TIM1, TIM_IT_Update);

    // 4. 配置 NVIC
    NVIC_InitStructure.NVIC_IRQChannel = TIM1_UP_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 1;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);

    // 5. 使能更新中断
    TIM_ITConfig(TIM1, TIM_IT_Update, ENABLE);
    TIM_Cmd(TIM1,ENABLE);
}

/*
 * I2S2 主机发送（32bit）：MCU 出 BCLK/LRCK，发给 ES9018
 *   PB12 WS  → 帧时钟（复用推挽）
 *   PB13 CK  → 位时钟（复用推挽）
 *   PB15 SD  → 串行数据（复用推挽）
 * ES9018 用自己 24.576M 晶振作 MCLK，故 MCK 不输出。
 *
 * SPI DATAR 16bit，每个 32bit 样点拆 2 个半字，先高 16 后低 16。
 */
void I2S2_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    I2S_InitTypeDef  I2S_InitStructure = {
        .I2S_Mode = I2S_Mode_MasterTx,            /* 主机发送 */
        .I2S_Standard = I2S_Standard_Phillips,
        .I2S_DataFormat = I2S_DataFormat_32b,     /* 32bit */
        .I2S_MCLKOutput = I2S_MCLKOutput_Disable,
        .I2S_AudioFreq = I2S_AudioFreq_48k,
        .I2S_CPOL = I2S_CPOL_High
    };

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_SPI2, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO | RCC_APB2Periph_GPIOB, ENABLE);

    /* WS/CK/SD：主机全部输出 */
    GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_12 | GPIO_Pin_13 | GPIO_Pin_15;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_AF_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    SPI_I2S_DeInit(SPI2);
    I2S_Init(SPI2, &I2S_InitStructure);
}

static const uint16_t *s_i2s_buf;
static uint16_t s_i2s_len;

/*
 * DMA1_CH5 = SPI2_TX，内存 → SPI2.DATAR（发送）
 * 32bit 样点拆成 [hi][lo] 半字流，halfword_count = 样点数 × 2。
 */
void I2S2_DMA_Init(const uint16_t *buf, uint16_t halfword_count)
{
    DMA_InitTypeDef DMA_InitStructure = {0};

    s_i2s_buf = buf;
    s_i2s_len = halfword_count;

    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);

    DMA_DeInit(DMA1_Channel5);
    DMA_InitStructure.DMA_PeripheralBaseAddr = (uint32_t)&SPI2->DATAR;
    DMA_InitStructure.DMA_MemoryBaseAddr     = (uint32_t)buf;
    DMA_InitStructure.DMA_DIR                = DMA_DIR_PeripheralDST;
    DMA_InitStructure.DMA_BufferSize         = halfword_count;
    DMA_InitStructure.DMA_PeripheralInc      = DMA_PeripheralInc_Disable;
    DMA_InitStructure.DMA_MemoryInc          = DMA_MemoryInc_Enable;
    /* DATAR 16bit，I2S 也是 16bit */
    DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_HalfWord;
    DMA_InitStructure.DMA_MemoryDataSize     = DMA_MemoryDataSize_HalfWord;
    DMA_InitStructure.DMA_Mode               = DMA_Mode_Circular;
    DMA_InitStructure.DMA_Priority           = DMA_Priority_VeryHigh;
    DMA_InitStructure.DMA_M2M                = DMA_M2M_Disable;
    DMA_Init(DMA1_Channel5, &DMA_InitStructure);

    SPI_I2S_DMACmd(SPI2, SPI_I2S_DMAReq_Tx, ENABLE);
}

void I2S2_DMA_Start(void)
{
    SPI_I2S_ClearFlag(SPI2, I2S_FLAG_UDR);
    SPI_I2S_ClearFlag(SPI2, SPI_I2S_FLAG_OVR);

    DMA_Cmd(DMA1_Channel5, DISABLE);
    DMA_SetCurrDataCounter(DMA1_Channel5, s_i2s_len);
    DMA1_Channel5->MADDR = (uint32_t)s_i2s_buf;
    DMA_Cmd(DMA1_Channel5, ENABLE);

    I2S_Cmd(SPI2, ENABLE);
}


uint8_t I2S2_Underrun(void)
{
    return (SPI_I2S_GetFlagStatus(SPI2, I2S_FLAG_UDR) != RESET);
}

void I2S2_DMA_Stop(void)
{
    I2S_Cmd(SPI2, DISABLE);
    DMA_Cmd(DMA1_Channel5, DISABLE);
}

