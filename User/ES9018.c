#include "debug.h"
#include "ES9018.h"
#include "stdbool.h"
void ES9018_WriteReg(uint8_t reg, uint8_t value){
    while(I2C_GetFlagStatus(I2C2, I2C_FLAG_BUSY) != RESET);

    I2C_GenerateSTART(I2C2, ENABLE);
    while(!I2C_CheckEvent(I2C2, I2C_EVENT_MASTER_MODE_SELECT));

    I2C_Send7bitAddress(I2C2, ES9018_ADDR, I2C_Direction_Transmitter);
    while(!I2C_CheckEvent(I2C2, I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED));

    I2C_SendData(I2C2, reg);
    while(I2C_GetFlagStatus(I2C2, I2C_FLAG_TXE) == RESET);

    I2C_SendData(I2C2, value);
    while(!I2C_CheckEvent(I2C2, I2C_EVENT_MASTER_BYTE_TRANSMITTED));

    I2C_GenerateSTOP(I2C2, ENABLE);
}

uint8_t ES9018_ReadReg(uint8_t reg){
    uint8_t value;

    while(I2C_GetFlagStatus(I2C2, I2C_FLAG_BUSY) != RESET);

    I2C_GenerateSTART(I2C2, ENABLE);
    while(!I2C_CheckEvent(I2C2, I2C_EVENT_MASTER_MODE_SELECT));

    I2C_Send7bitAddress(I2C2, ES9018_ADDR, I2C_Direction_Transmitter);
    while(!I2C_CheckEvent(I2C2, I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED));

    I2C_SendData(I2C2, reg);
    while(I2C_GetFlagStatus(I2C2, I2C_FLAG_TXE) == RESET);

    I2C_GenerateSTART(I2C2, ENABLE);
    while(!I2C_CheckEvent(I2C2, I2C_EVENT_MASTER_MODE_SELECT));

    I2C_Send7bitAddress(I2C2, ES9018_ADDR, I2C_Direction_Receiver);
    while(!I2C_CheckEvent(I2C2, I2C_EVENT_MASTER_RECEIVER_MODE_SELECTED));

    I2C_AcknowledgeConfig(I2C2, DISABLE);
    while(I2C_GetFlagStatus(I2C2, I2C_FLAG_RXNE) == RESET);

    value = I2C_ReceiveData(I2C2);
    I2C_GenerateSTOP(I2C2, ENABLE);
    return value;
}

void ES9018_SoftwareReset(void)
{
    u8 temp = ES9018_ReadReg(0x00);
    // 软复位特殊处理：写入后芯片立即复位，不能等待ACK
    while(I2C_GetFlagStatus(I2C2, I2C_FLAG_BUSY) != RESET);

    I2C_GenerateSTART(I2C2, ENABLE);
    while(!I2C_CheckEvent(I2C2, I2C_EVENT_MASTER_MODE_SELECT));

    I2C_Send7bitAddress(I2C2, ES9018_ADDR, I2C_Direction_Transmitter);
    while(!I2C_CheckEvent(I2C2, I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED));

    I2C_SendData(I2C2, 0x00);  // 寄存器地址
    while(I2C_GetFlagStatus(I2C2, I2C_FLAG_TXE) == RESET);

    I2C_SendData(I2C2, temp | 0x01);  // 软复位命令
    Delay_Ms(1);               // 等待芯片开始复位

    I2C_GenerateSTOP(I2C2, ENABLE);
    Delay_Ms(1);             // 等待复位完成
}

void ES9018_HardwareReset(void)
{
    GPIO_ResetBits(GPIOB, GPIO_Pin_14);  // 拉低复位
    Delay_Ms(1);
    GPIO_SetBits(GPIOB, GPIO_Pin_14);    // 拉高释放
    Delay_Ms(1);
}

/*
 * 快速复位：只拉一下 RST 再放开，**不重写任何寄存器**
 * （芯片复位后按硬件默认/strap 就能直接工作）。
 *
 * 为什么用 Delay_Us 而不是 Delay_Ms：
 *   1) 总耗时 ~210us，而不是 2ms；
 *   2) Delay_Us 是轮询 SysTick 的循环，**可以被中断打断** ——
 *      USB 中断该来还是来，音频同步包不会被错过。
 * 调用点：Audio_SyncTick()（TIM2 中断）里，见 usb_app.c 的 s_es9018_rst_req。
 */
void ES9018_FastReset(void)
{
    GPIO_ResetBits(GPIOB, GPIO_Pin_14);   /* RST 拉低 */
    Delay_Us(10);                         /* 复位脉冲（最短即可） */
    GPIO_SetBits(GPIOB, GPIO_Pin_14);     /* 释放 */
    Delay_Us(200);                        /* 等芯片内部起来 */
}

void ES9018_Init(void)
{

    ES9018_HardwareReset();
    ES9018_SoftwareReset();
    
    /*
     * Reg0x01：32bit + I2S，固定 I2S 输入（auto=0）。
     * MCU 为 I2S 主机，ES9018 作从机接收。
     */
    ES9018_WriteReg(ES9018_REG_INPUT_CONFIG, 0xE0); /* 32bit, I2S, 固定 I2S */
    ES9018_WriteReg(0x02, 0x18);
    ES9018_WriteReg(0x03, 0x10);
    ES9018_WriteReg(0x04, 0x00);
    ES9018_WriteReg(0x05, 0x68);
    ES9018_WriteReg(0x06, 0x4A);
    ES9018_WriteReg(ES9018_REG_GENERAL_SETTINGS, 0x80 | ES9018_FILTER_SLOW_ROLLOFF); /* 不 mute */
    ES9018_WriteReg(0x08, 0x10);

    /*
     * ================= Reg0x0A（Master Mode Control）**不要写** =================
     *
     * 这一行原来是 ES9018_WriteReg(ES9018_REG_MASTER_MODE_CTRL, 0x00);
     * 当时的想法是"Bit 时钟又不是 ES9018 驱动的，这个寄存器无所谓" —— **错了**。
     *
     * 手册 Register #10：默认值 = **0x5**，其中
     *   [7]   master_clock_enable  = 0（不输出 BCLK/LRCK，我们是 MCU 当主机，这部分确实无所谓）
     *   [6:5] clock_divider_select = 0
     *   [4]   sync_mode            = 0
     *   [3:0] stop_div             = 5  ← **关键位**
     *
     * stop_div = "DPLL 和 ASRC 锁定前必须经过多少个 FSR 边沿"：
     *     4'd0  = 16384 个 FSR 边沿
     *     4'd5  =  2730 个（芯片默认值）
     *     …
     *     4'd15 =  1024 个
     *
     * 写 0x00 把 stop_div 从 5 改成了 0 —— 锁定时要等的边沿数变成 6 倍。
     * 于是 DPLL/ASRC 的锁定行为被改坏，听感就是**爆米花一样的劈啪声**
     * （不同步、反复失锁/重捕）。实测：把这行注释掉、用芯片默认的 0x5，问题消失。
     *
     * 结论：**这个寄存器的锁定 FSR 数量很重要，别随便写，用默认值就行。**
     * 下面保留注释而不是删掉，就是为了留下这个教训：
     *   // ES9018_WriteReg(ES9018_REG_MASTER_MODE_CTRL, 0x00);
     *
     * 注意这跟 master/slave 无关：我们确实不需要它输出时钟，
     * 但 stop_div 影响的是**内部 DPLL 的锁定速度**，和谁输出 BCLK 是两回事。
     */

    ES9018_WriteReg(0x0B, 0x02);
    ES9018_WriteReg(0x0C, 0x5A);
    ES9018_WriteReg(0x0D, 0x00);//使能THD补偿
    ES9018_WriteReg(0x0E, 0x8A |  ES9018_SOFT_START_ON_LOCK);
    ES9018_WriteReg(0x0F, 0x00); /* 音量 0dB */
    ES9018_WriteReg(0x10, 0x00);
    ES9018_WriteReg(0x11, 0xFF);
    ES9018_WriteReg(0x12, 0xFF);
    ES9018_WriteReg(0x13, 0xFF);
    ES9018_WriteReg(0x14, 0x7F);
    ES9018_WriteReg(0x15, 0x00);

    Delay_Ms(20);
}

void ES9018_SetVolume(uint8_t vol)
{
    ES9018_WriteReg(ES9018_REG_VOLUME_1, vol);
    ES9018_WriteReg(ES9018_REG_VOLUME_2, vol);
}

void ES9018_SetVolumeL(uint8_t vol)
{
    ES9018_WriteReg(ES9018_REG_VOLUME_1, vol);
}

void ES9018_SetVolumeR(uint8_t vol)
{
    ES9018_WriteReg(ES9018_REG_VOLUME_2, vol);
}

void ES9018_Mute(uint8_t mute)
{
    uint8_t val = ES9018_ReadReg(ES9018_REG_GENERAL_SETTINGS);
    val &= ~ES9018_MUTE_BOTH;
    val |= (mute & ES9018_MUTE_BOTH);
    ES9018_WriteReg(ES9018_REG_GENERAL_SETTINGS, val);
}

void ES9018_SetFilterShape(uint8_t shape)
{
    uint8_t val = ES9018_ReadReg(ES9018_REG_GENERAL_SETTINGS);
    val &= ~(3 << 5);
    val |= shape;
    ES9018_WriteReg(ES9018_REG_GENERAL_SETTINGS, val);
}

void ES9018_SetInputSelect(uint8_t sel)
{
    uint8_t val = ES9018_ReadReg(ES9018_REG_INPUT_CONFIG);
    val &= ~3;
    val |= (sel & 3);
    ES9018_WriteReg(ES9018_REG_INPUT_CONFIG, val);
}

void ES9018_SetI2SFormat(uint8_t length, uint8_t mode)
{
    /* length/mode 传入已是移位后的宏（如 ES9018_I2S_LENGTH_32BIT） */
    uint8_t val = ES9018_ReadReg(ES9018_REG_INPUT_CONFIG);
    val &= ~((3 << 6) | (3 << 4));
    val |= (length & (3 << 6));
    val |= (mode & (3 << 4));
    ES9018_WriteReg(ES9018_REG_INPUT_CONFIG, val);
}

uint8_t ES9018_ReadChipStatus(void)
{
    return ES9018_ReadReg(ES9018_REG_CHIP_STATUS);
}

uint8_t ES9018_ReadLockStatus(void)
{
    return (ES9018_ReadReg(ES9018_REG_CHIP_STATUS) & 1);
}

void ES9018_SetBitCLKDIV(uint8_t div){
    u8 val = ES9018_ReadReg(ES9018_REG_MASTER_MODE_CTRL) & 0b10011111;
    switch(div)
    {
        case 4:
            ES9018_WriteReg(ES9018_REG_MASTER_MODE_CTRL, val | ES9018_CLK_DIV_MCLK_4); 
            break;
        case 8:
            ES9018_WriteReg(ES9018_REG_MASTER_MODE_CTRL, val | ES9018_CLK_DIV_MCLK_8); 
            break;
        case 16:
            ES9018_WriteReg(ES9018_REG_MASTER_MODE_CTRL, val | ES9018_CLK_DIV_MCLK_16); 
            break;
        default:
            ES9018_WriteReg(ES9018_REG_MASTER_MODE_CTRL, val | ES9018_CLK_DIV_MCLK_4); 
            break;
    }
}