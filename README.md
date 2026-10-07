# LittleTail

CH32V305 + ES9018 的 **USB Type-C → 3.5mm AUX 转接器**固件（**多采样率模板工程**）。
Windows 上开发调通，支持 24k / 48k / 96k，32bit 立体声播放。

> 这是**母工程 / 模板**，保留多采样率结构和全部调试手段。
> 面向 Android 平板、只做 48k 的那份是独立的 **[LittleTail48k](https://github.com/Kreeli/LittleTail48k)**。

---

## 目录里有三个工程，别搞混

| 目录 | 定位 | 说明 |
|---|---|---|
| `LittleTail`（本仓库） | **多采样率模板** | 24k/48k/96k，32bit，代码带大量中文注释，保留 CDC 调试打印 |
| `LittleTail48k` | **平板专用版** | 只报 48k、16bit 帧 + **精确 48000.000Hz**、不依赖反馈、无调试打印、状态全靠 LED |
| `LittleTail_template` | 更早的副本 | 与 `LittleTail` 同一个 remote，历史遗留，可忽略 |

---

## 当前配置一览

### 硬件 / 引脚

| 项 | 值 |
|---|---|
| MCU | CH32V305FBP6（RV32IMAFC @144MHz，USBHS 高速设备） |
| DAC | ES9018K2M（I2C 从机配置；I2S 从机；自带 24.576MHz 晶振作 MCLK） |
| I2S | SPI2/I2S2 **主机发送**，PB12=WS / PB13=CK / PB15=SD |
| DMA | DMA1_Channel5，循环模式，**永不停止**（DAC 始终有时钟） |
| I2C | I2C2，PB10=SCL / PB11=SDA，ES9018 地址 0x90 |
| 复位 | PB14 = ES9018 RST，PA5 = 耳放 RST |
| 指示 | PC7 = 采样率灯 / 错误闪灯，PC8 = LED |

### 时钟与采样率

```
HSE 8MHz --(PREDIV2 不分频)--> 8MHz --(PLL3MUL=10)--> PLL3CLK 80MHz
        --> PLL3_VCO = 2x = 160MHz = I2SxCLK
```

| 参数 | 值 |
|---|---|
| I2S 帧格式 | **32bit 帧**（每帧 64 BCLK），`I2S_DataFormat_32b` |
| 分频公式 | `Fs = I2SxCLK / (64*N)`，`N = 2*I2SDIV + ODD` |
| 真实 LRCK @48k | `160e6/(64*52)` = **48076.92 Hz**（+0.16%，**不是精确 48000**） |
| 96k | N=26 → 96153.85Hz |

> ⚠ 32bit 帧在 8MHz HSE 下**没有任何精确 48000 解**（所有 PREDIV2 × PLL3MUL 组合都枚举过）。
> 只有 16bit 帧（32 BCLK/帧）存在精确解。这是本工程与 48k 版最大的架构差异。

### 音频缓冲与同步

| 参数 | 值 | 说明 |
|---|---|---|
| `I2S_BUF_HALFWORDS` | 3072 | = 768 帧 = **16ms** @48k（32bit 时 4 半字/帧） |
| `SYNC_TARGET_MS` | 4 | 水位目标 4ms |
| `SYNC_MARGIN_MS` | 2 | 安全区 2ms |
| `SYNC_KP_DEFAULT` | 0.001 | 比例增益 |
| `SYNC_KI_DEFAULT` | 0.0004 | 积分增益（时间常数约 5 秒） |
| `SYNC_CORR_LIMIT` | 0.25 | 修正限幅 ±0.25 帧/微帧 = ±2% |
| `SYNC_HI_GUARD` | 128 | 深水位保护余量（半字） |
| 同步节拍 | TIM2 **1kHz 中断** | 不在主循环里跑 |

### USB 描述符

| 项 | 值 |
|---|---|
| 类 | 复合设备：CDC-ACM(虚拟串口) + UAC2 音频，`bDeviceClass=0xEF/02/01` |
| VID/PID | `0x1209` / `0x0000`（pid.codes 测试 VID） |
| 数据端点 | `0x03`，`bmAttributes=0x05`（isochronous + asynchronous） |
| 反馈端点 | `0x84`，4 字节，**16.16 定点**，单位"每微帧样点数" |
| `bInterval` | **1**（= 每微帧一包，125µs）⚠ 高速等时端点的 bInterval 是**指数**：`2^(n-1)` 微帧 |
| 采样率 | 24k / 48k / 96k（Clock Source 的 RANGE 表） |
| `wMaxPacketSize` | 1024 字节 |
| 采样率数 | `wRES` 目前写 **1**（见下文"已知问题"） |

### 调试输出（CDC）

本工程**保留了** CDC 打印（48k 版已整体编译掉）：

- 每 50ms 一行遥测：`水位,目标,反馈值,标称,修正,误差,Kp,Ki,欠载,包数,实时水位,累计丢包`
- 可用串口在线改增益：发 `p=0.001` / `i=0.0004` / `d=0`
- PC7 灯：96k 灭、其它亮（采样率指示）

---

## 已知问题 / 与 48k 版的差距

这份是**较早的版本**，后来在 [LittleTail48k](https://github.com/Kreeli/LittleTail48k) 里
定位并修掉的问题，**本工程尚未同步**。要当作可靠模板用，建议先把下面这些搬过来：

| # | 问题 | 后果 | 48k 版的做法 |
|---|---|---|---|
| 1 | 采样率表 `dRES=1` | Linux/安卓的 `parse_uac2_sample_rate_range()` 把 `res==1` 当"连续范围"并**立刻 return**，后续档位全丢 → 安卓只认出 24000 一档 | `dRES` 写 **0** |
| 2 | LRCK 是 48076.92Hz（+0.16%） | 主机若不读反馈，水位每秒被抽 77 帧 → 周期性断音 | 16bit 帧凑**精确 48000.000Hz**，开环稳定 |
| 3 | `Ki=0.0004`（时间常数约 5 秒） | 积分器来不及在两个欠载之间吃掉残余偏差 | `Ki=0.004`，并**预置积分项**而非硬写输出 |
| 4 | 水位余量只有 2ms | 主机侧几十 ms 的调度抖动就会欠载 | 水位 16ms、缓冲 64ms |
| 5 | 欠载恢复按"目标水位"整段填 0 | 每次欠载硬插 16ms 静音（每秒被"砍一刀"） | 加 `gap` 参数，只留 2ms |
| 6 | 流停 3ms 就 `memset` 整个环形缓冲 | 丢掉 10~30ms 有效音频，且在 TIM2 中断里阻塞 USB | 只清"播放位置之后一小段" |
| 7 | 没有逐帧上溢保护 | 写指针万一越过读指针会输出满幅噪声 | 逐帧判定写指针不得越过读指针 + ES9018 `mute_on_lock` |
| 8 | `bInterval=1` 时 URB 8000/s | 安卓主机在忙时容易延迟提交 | `bInterval=4`（1ms 一包）降到 1000/s |

> 修这些问题时的完整推导、踩坑记录和取舍，都写在 48k 版的 README 与代码注释里。

---

## 编译

MounRiver Studio 2 打开 `LittleTail.wvproj`；或命令行：

```
make -C obj main-build     # 需要 RISC-V GCC 与 make 在 PATH
```

---

## 目录结构

```
User/          usb_app.c   音频同步环、ISO 回调、反馈端点、采样率/音量处理
               Syscfg.c    I2S + DMA + PLL3 时钟 + TIM2 同步定时器
               ES9018.c/d  DAC 初始化（I2C）
               usbd_desc.h 全部 UAC2 描述符
               ch32v30x_it.c  USBHS / TIM2 中断入口
Cherry_USB/    USB 协议栈（CherryUSB，含 WCH USBHS 端口层）
Peripheral/    CH32V30x 标准外设库
Core/ Startup/ Ld/   RISC-V 内核支持、启动文件、链接脚本
tools/uac2_audit/    从编译产物里解析描述符做审计的脚本
_pdfwork/            从 PDF 手册提取文本的工具链
```

## License

未指定。CherryUSB 与 CH32V30x 外设库版权归各自作者所有。
