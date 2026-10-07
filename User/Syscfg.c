#include "Syscfg.h"
void Syscfg(void){
	LED_Init();
	I2C2_init();
	ES9018_RST_Init();
	HP_AMP_RST_Init();
    TIM1_Init();
    AUDIO_SYNC_TIM_Init();
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
    /* 中断优先级规划（数字越小越优先）：
     *   USBHS   preempt=0   一个微帧 125us，绝不能被拖
     *   TIM2    preempt=1   1kHz 音频同步环（反馈 PID + 水位对齐）
     *   TIM1    preempt=3   现在只清标志（PC7 已改成采样率指示，不再翻转）
     *   DMA1_CH5 没开中断（不需要），handler 是空的
     * USB 和 TIM2 的优先级分别在 usb_app.c / AUDIO_SYNC_TIM_Init 里设置。 */
    NVIC_InitStructure.NVIC_IRQChannel = TIM1_UP_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 3;
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
    /* I2S_Init 内部按 SYSCLK 算 I2SPR，对 PLL3_VCO 源是错的。
       这里传 Default 让它只写 I2SCFGR，分频交给 I2S_SetFs。 */
    I2S_InitStructure.I2S_AudioFreq = I2S_AudioFreq_Default;
    I2S_Init(SPI2, &I2S_InitStructure);
    I2S_SetFs(48000);
}

// void I2S2_Init(void)
// {
//     GPIO_InitTypeDef GPIO_InitStructure = {0};
//     I2S_InitTypeDef  I2S_InitStructure = {
//         .I2S_Mode = I2S_Mode_SlaveTx,            /* 主机发送 */
//         .I2S_Standard = I2S_Standard_Phillips,
//         .I2S_DataFormat = I2S_DataFormat_32b,     /* 32bit */
//         .I2S_MCLKOutput = I2S_MCLKOutput_Disable,
//         .I2S_AudioFreq = I2S_AudioFreq_96k,
//         .I2S_CPOL = I2S_CPOL_High
//     };

//     RCC_APB1PeriphClockCmd(RCC_APB1Periph_SPI2, ENABLE);
//     RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO | RCC_APB2Periph_GPIOB, ENABLE);

//     /* WS/CK：主机入 */
//     GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_12 | GPIO_Pin_13;
//     GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_IPD;
//     GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
//     GPIO_Init(GPIOB, &GPIO_InitStructure);

//     GPIO_InitStructure.GPIO_Pin   =  GPIO_Pin_15;
//     GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_AF_PP;
//     GPIO_Init(GPIOB, &GPIO_InitStructure);

//     I2S_Init(SPI2, &I2S_InitStructure);
// }

void I2S2_ClockInit(void)
{
    uint32_t guard = 1000000u;
    //RCC_PREDIV2Config(RCC_PREDIV2_Div1); /* 8MHz / 1 = 4MHz */
    /* VCO = 8MHz * 10 * 2 = 160MHz（VCO 输出是 ×2）
     * 160M 对 48k/96k/192k 分频比 52/26/13，误差都是 +0.16%
     * 不要用 Mul_6(96M)：N=31.25/15.625/7.8125 不整，96k/192k 会偏 -2.34% */
    RCC_PLL3Config(RCC_PLL3Mul_10);
    RCC_PLL3Cmd(ENABLE);
    while ((RCC_GetFlagStatus(RCC_FLAG_PLL3RDY) == RESET) && (guard-- != 0u)) {
    }
    RCC_I2S2CLKConfig(RCC_I2S2CLKSource_PLL3_VCO);
}

/*
 * 按 I2S2 实际时钟源写 I2SPR，设定 FRAME（LRCK）频率。
 *
 * 时钟：I2S2CLK = PLL3_VCO = 160 MHz（见 I2S2_ClockInit，不是 SYSCLK 144 MHz）
 * 格式：32bit × 2ch → 每帧 64 个 BCLK，MCLK 不输出（ES9018 自带晶振）
 *
 *   BCLK = fI2S / (2*I2SDIV + ODD)
 *   Fs   = BCLK / 64 = fI2S / (64 * (2*I2SDIV + ODD))
 *   => N = 2*I2SDIV + ODD ≈ fI2S / (64 * Fs)
 */
#define I2S2_SRC_HZ  160000000u

/* 当前分频比 N = 2*I2SDIV + ODD（I2SPR: [7:0]=I2SDIV, [8]=ODD） */
uint16_t I2S_GetDivN(void)
{
    uint16_t pr = (uint16_t)SPI2->I2SPR;

    return (uint16_t)(((pr & 0xFFu) << 1) | ((pr >> 8) & 1u));
}

/* 真实的 LRCK 频率：fS = fI2S / (64 * N) */
uint32_t I2S_GetRealFs(void)
{
    uint16_t n = I2S_GetDivN();

    if (n == 0u) {
        return 0u;
    }
    return (uint32_t)(I2S2_SRC_HZ / (64u * (uint32_t)n));
}

uint8_t I2S_SetFs(uint32_t freq)
{
    uint32_t n;
    uint16_t i2sodd, i2sdiv, pr;
    uint16_t cfgr;

    if (freq == 0u) {
        return 0u;
    }

    /* N = round(fI2S / (64 * Fs)) */
    n = (uint32_t)(((uint64_t)I2S2_SRC_HZ + 32u * (uint64_t)freq) / (64u * (uint64_t)freq));

    i2sodd = (uint16_t)(n & 1u);
    i2sdiv = (uint16_t)(n / 2u);

    /* 硬件合法范围：I2SDIV ∈ [2, 255] */
    if ((i2sdiv < 2u) || (i2sdiv > 0xFFu)) {
        i2sdiv = 2u;
        i2sodd = 0u;
    }

    pr = (uint16_t)(i2sdiv | (uint16_t)(i2sodd << 8) | I2S_MCLKOutput_Disable);

    /*
     * **分频没变就什么都别做**！！
     *
     * 主机每次打开音频流都会发 SET_CUR(SAMPLING_FREQ)（哪怕采样率根本没变），
     * 而下面那段"关 I2S → 写分频 → 恢复 I2SE"会让 I2S **关一下又开**：
     * 帧相位（哪个半字是 L、哪个是 R）就从当前位置随便重新开始了，而 DMA 的
     * 半字流一点没动 —— 两者永久错开，表现为：
     *   时好时坏 / 左右声道互换 / 没声 / 满幅噪声，而且和 CPOL 无关。
     * 所以先比较分频：一样就直接返回，绝不碰 I2SE。
     *
     * 返回值：0 = 分频没变（I2S 没被打断）；1 = 真的改了（帧相位已断，调用方要重建流）
     */
    if ((SPI2->I2SPR & 0x01FFu) == (pr & 0x01FFu)) {
        return 0u;                    /* 高频路径：采样率没变，保持帧相位不动 */
    }

    /* 真的要改分频：只能关掉再写（手册要求 I2SDIV/ODD 只能在 I2SE=0 时写），
     * 写完恢复 I2SE。调用方（usbd_audio_set_sampling_freq）必须随后把整条流
     * 对齐重启，否则帧相位就是随机值。 */
    cfgr = SPI2->I2SCFGR;
    SPI2->I2SCFGR = (uint16_t)(cfgr & (uint16_t)~0x0400); /* I2SE = 0 */
    SPI2->I2SPR = pr;
    SPI2->I2SCFGR = cfgr;

    return 1u;
}

/*
 * DMA1_CH5 = SPI2_TX，内存 → SPI2.DATAR（发送）
 * 32bit 样点拆成 [hi][lo] 半字流，halfword_count = 样点数 × 2。
 */void I2S2_DMA_Init(const uint16_t *buf, uint16_t halfword_count)
{
    DMA_InitTypeDef DMA_InitStructure = {0};


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

void I2S2_DMA_Start(const uint16_t *buf, uint16_t halfword_count)
{
    uint32_t guard = 200000u;

    SPI_I2S_ClearFlag(SPI2, I2S_FLAG_UDR);
    SPI_I2S_ClearFlag(SPI2, SPI_I2S_FLAG_OVR);

    DMA_Cmd(DMA1_Channel5, DISABLE);
    DMA_SetCurrDataCounter(DMA1_Channel5, halfword_count);
    DMA1_Channel5->MADDR = (uint32_t)buf;
    DMA_Cmd(DMA1_Channel5, ENABLE);

    /*
     * **先等 DMA 把第一个半字搬进 SPI 的 TX 缓冲，再使能 I2S。**
     *
     * 这一步是"帧相位确定性"的关键：如果 I2S 先开、TX 缓冲还是空的，
     * 第一帧会先吐一个 0（或残留值），于是整条流永久错开一个半字 ——
     * 表现就是"有时候正常、有时候声道错乱/噪声"，而且每次上电结果都可能不同。
     * 等到了再开，第一个半字一定是 buf[0]，帧边界从它开始。
     * 如果硬件在 I2SE=0 时不产生 DMA 请求，这里会退化成原来的行为（最多等 ~200us）。
     */
    while ((DMA_GetCurrDataCounter(DMA1_Channel5) == halfword_count) && (--guard != 0u)) {
    }

    I2S_Cmd(SPI2, ENABLE);
}

/*
 * 彻底重建 I2S 外设（采样率切换后必须走这条）：
 *   SPI_I2S_DeInit() 是**外设复位**，会把 SPI 的 TX 缓冲/移位寄存器等内部状态
 *   全部清掉；只改 I2SPR 是不够的 —— 残留的半个字会让新流错开一格，
 *   听起来就是"切换采样率之后波形又乱了"。
 *
 * 结束时 **I2S 保持关闭**，由 I2S2_DMA_Start() 在"第一个半字已经进 TX 缓冲"
 * 之后再使能，保证帧相位从缓冲第 0 个半字开始。
 */
void I2S2_Reinit(uint32_t freq)
{
    I2S_InitTypeDef cfg = {
        .I2S_Mode = I2S_Mode_MasterTx,
        .I2S_Standard = I2S_Standard_Phillips,
        .I2S_DataFormat = I2S_DataFormat_32b,
        .I2S_MCLKOutput = I2S_MCLKOutput_Disable,
        .I2S_AudioFreq = I2S_AudioFreq_Default,   /* 分频自己算，见 I2S_SetFs */
        .I2S_CPOL = I2S_CPOL_High                 /* 基准值，和 I2S2_Init 保持一致 */
    };

    I2S_Cmd(SPI2, DISABLE);
    DMA_Cmd(DMA1_Channel5, DISABLE);

    SPI_I2S_DeInit(SPI2);                 /* 外设复位：内部状态全清 */
    I2S_Init(SPI2, &cfg);                 /* 只写 I2SCFGR；I2SPR 由下面算 */
    (void)I2S_SetFs(freq);                /* 写分频（这里会关/开一次，无所谓） */
    SPI_I2S_DMACmd(SPI2, SPI_I2S_DMAReq_Tx, ENABLE);  /* 外设复位会清掉 DMA 请求使能 */

    I2S_Cmd(SPI2, DISABLE);               /* 明确关着，等 DMA 先填第一个半字 */
}


/*
 * 原来这里有一个 I2S2_Underrun()（读 I2S_FLAG_UDR），从未被调用过，已删除。
 *
 * 关于 UDR：曾怀疑"劈里啪啦"是 I2S 发送下溢引起的（手册 20.3.6.4 说下溢后
 * CHSIDE 无效、必须把 I2S 关闭再打开才能恢复，形状很像），于是在 TIM2 中断里
 * 加了 UDR 检测 + PC9 锁存指示。**实测 PC9 从未点亮** —— 排除了 I2S 发送路径。
 * 真正的原因是 ES9018 的 Reg0x0A（Master Mode Control）被写成了 0x00，
 * 把 stop_div 从默认的 5（2730 个 FSR 边沿）改成 0（16384 个），
 * 锁相行为被改坏 → 爆米花声。现在这一写已被注释掉，用芯片默认值。
 * 详见 ES9018.c 里 Reg0x0A 处的说明。
 */

void I2S2_DMA_Stop(void)
{
    I2S_Cmd(SPI2, DISABLE);
    DMA_Cmd(DMA1_Channel5, DISABLE);
}

/* ---------------------------------------------------------------------------
 * 音频同步环的节拍定时器：TIM2 更新中断 @1kHz
 *
 * 为什么不用主循环延时：主循环会被 printf/CDC 输出拖慢到几十毫秒，节拍完全不准；
 * 反馈值（UAC2 异步播放的水位 PI）必须按固定周期更新。
 * 参考 TIM1 的配置方式（见上面的 TIM1_Init）。
 *
 * 时钟：TIM1 在 APB2 上跑 144MHz（你的 TIM1_Init 用 14400 分频得到 10kHz，
 * 正好说明它的定时器时钟是 144MHz）。TIM2 在 APB1 上，同样是 144MHz（见下）。
 * ------------------------------------------------------------------------- */
/* TIM2 的时钟：CH32V3 上 APB1 分频系数 ≠1 时，定时器时钟 = APB1 × 2。
 * APB1 = 72MHz -> TIM2 = **144MHz**（不是 72MHz）。
 * 实测确认：bInterval=1 时主机每毫秒 8 个包，而 S, 行最后一列读到 4，
 * 说明临时中断频率是 1kHz 的 2 倍 —— 之前这里写 72MHz，实际 tick 是 2kHz，
 * PID 里的 dt 也就错了一倍（积分作用偏强一倍）。改成 144MHz 后 tick 才是真 1kHz。 */
#define SYNC_TIMER_CLK_HZ   144000000u

void AUDIO_SYNC_TIM_Init(void)
{
    NVIC_InitTypeDef NVIC_InitStructure = {0};
    TIM_TimeBaseInitTypeDef TIM_TimeBaseInitStructure = {0};

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM2, ENABLE);

    /* 先分频到 1MHz，再数 1000 个 -> 1kHz 更新中断 */
    TIM_TimeBaseInitStructure.TIM_Period = 1000 - 1;
    TIM_TimeBaseInitStructure.TIM_Prescaler = (SYNC_TIMER_CLK_HZ / 1000000u) - 1;
    TIM_TimeBaseInitStructure.TIM_ClockDivision = TIM_CKD_DIV1;
    TIM_TimeBaseInitStructure.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInitStructure.TIM_RepetitionCounter = 0;
    TIM_TimeBaseInit(TIM2, &TIM_TimeBaseInitStructure);

    TIM_ClearITPendingBit(TIM2, TIM_IT_Update);

    /* 抢占优先级要低于 USBHS(0)，别把 USB 中断挤掉 */
    NVIC_InitStructure.NVIC_IRQChannel = TIM2_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 1;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);

    TIM_ITConfig(TIM2, TIM_IT_Update, ENABLE);
    TIM_Cmd(TIM2, ENABLE);
}

