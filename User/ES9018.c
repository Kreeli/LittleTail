#include "debug.h"
#include "ES9018.h"
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
    // 软复位特殊处理：写入后芯片立即复位，不能等待ACK
    while(I2C_GetFlagStatus(I2C2, I2C_FLAG_BUSY) != RESET);

    I2C_GenerateSTART(I2C2, ENABLE);
    while(!I2C_CheckEvent(I2C2, I2C_EVENT_MASTER_MODE_SELECT));

    I2C_Send7bitAddress(I2C2, ES9018_ADDR, I2C_Direction_Transmitter);
    while(!I2C_CheckEvent(I2C2, I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED));

    I2C_SendData(I2C2, 0x00);  // 寄存器地址
    while(I2C_GetFlagStatus(I2C2, I2C_FLAG_TXE) == RESET);

    I2C_SendData(I2C2, 0x01);  // 软复位命令
    Delay_Ms(1);               // 等待芯片开始复位

    I2C_GenerateSTOP(I2C2, ENABLE);
    Delay_Ms(100);             // 等待复位完成
}

void ES9018_HardwareReset(void)
{
    GPIO_ResetBits(GPIOB, GPIO_Pin_14);  // 拉低复位
    Delay_Ms(10);
    GPIO_SetBits(GPIOB, GPIO_Pin_14);    // 拉高释放
    Delay_Ms(100);
}

void ES9018_Init(void)
{

    ES9018_HardwareReset();

    /*
     * Reg0x01：必须 32bit + I2S，且 auto_input=0（固定 I2S）。
     * 若 auto_input=11，浮空的 SPDIF 脚可能被误切换，SD 上有数据也不出声。
     */
    ES9018_WriteReg(ES9018_REG_INPUT_CONFIG, 0x80); /* 32bit, I2S, 固定 I2S 输入 */

    Delay_Ms(100);
    ES9018_WriteReg(0x01, 0x8C);
    ES9018_WriteReg(0x02, 0x18);
    ES9018_WriteReg(0x03, 0x10);
    ES9018_WriteReg(0x04, 0x00);
    ES9018_WriteReg(0x05, 0x68);
    ES9018_WriteReg(0x06, 0x6A);
    ES9018_WriteReg(ES9018_REG_GENERAL_SETTINGS, 0x80 | ); /* 不 mute */
    ES9018_WriteReg(0x08, 0x10);
    ES9018_WriteReg(0x09, 0x22);

    /* Reg0x0A：主模式，MCLK/8 → 48kHz（24.576M/8/64） */
    ES9018_WriteReg(ES9018_REG_MASTER_MODE_CTRL, 0xA5);

    ES9018_WriteReg(0x0B, 0x02);
    ES9018_WriteReg(0x0C, 0x5A);
    ES9018_WriteReg(0x0D, 0x40);
    ES9018_WriteReg(0x0E, 0x8A);
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
