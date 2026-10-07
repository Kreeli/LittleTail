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
| `SYNC_TARGET_MS` | **8** | 水位目标 8ms（96k 下受缓冲限制夹到 4ms） |
| `SYNC_MARGIN_MS` | **4** | 安全区 4ms |
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
| 采样率 | **48k / 96k**（Clock Source 的 RANGE 表）；24k 已移除 |
| `wMaxPacketSize` | 1024 字节 |
| 采样率数 | `wRES` 目前写 **1**（见下文"已知问题"） |

### 调试输出（CDC）

本工程**保留了** CDC 打印（48k 版已整体编译掉）：

- 每 50ms 一行遥测：`水位,目标,反馈值,标称,修正,误差,Kp,Ki,欠载,包数,实时水位,累计丢包`
- 可用串口在线改增益：发 `p=0.001` / `i=0.0004` / `d=0`
- PC7 灯：96k 灭、其它亮（采样率指示）

---

## 相对 48k 版的差距 / 已知情况

后来在 [LittleTail48k](https://github.com/Kreeli/LittleTail48k) 里定位到的问题，
本工程只挑了三项修（见下），**其余的一律保留原样**。

### 已修（本次）

| # | 问题 | 处理 |
|---|---|---|
| 1 | 支持 24k 采样率 | **移除**。采样率表只声明 **48k / 96k**（`SAMPLE_RATE` 枚举同步去掉 `FS_24000`）。低于 48k 的请求一律按 48k 处理 |
| 2 | 没有逐帧上溢保护 | **加上**。`Audio_out_callback()` 里每写一帧（4 半字）前先算"写指针到读指针还有多少空位（环形）"，不够就丢弃本包剩余并累加 `s_overflow` |
| 3 | 水位余量只有 2ms | **翻倍**：`SYNC_TARGET_MS 4 -> 8`、`SYNC_MARGIN_MS 2 -> 4`。注意 96k 下缓冲仅 8ms，目标会被夹到 4ms；48k 下是完整的 8ms |

> 关于 #2：#1 和 #3 都改变了水位/包长关系，而写指针一旦越过读指针，
> 水位 lvl 的环形运算会把"很满"读成"很小"，PI 反而让主机送得更快（反向死锁），
> 同时 DMA 播出被覆盖到一半的数据 = **满幅噪声**。这道逐帧保护是防这个的。
> 正常水位下空位有一半以上，判断永远不成立；触发次数可用串口遥测观察。

### 保留原样（属于本工程的特点，或已知但暂不处理）

| # | 情况 | 说明 |
|---|---|---|
| A | 采样率表 `dRES = 1` | Linux/安卓的 `parse_uac2_sample_rate_range()` 会把 `res==1` 当成"连续范围"并**立刻 return**，后续三元组全丢。**安卓上只认出第一档**。Windows 不受影响（本工程主要面向 Windows 调试）。48k 版已改 `dRES=0` |
| B | LRCK = 48076.92Hz（**+0.16%**，非精确 48000） | 32bit 帧在 8MHz HSE 下没有任何精确 48000 分频解（所有 PREDIV2 × PLL3MUL 组合都枚举过）。偏差靠 UAC2 异步反馈补偿；若主机不读反馈，水位每秒被抽 77 帧 |
| C | 32bit / 4 字节样点（**满分辨率，这是刻意的**） | 每声道 32 位送到 ES9018。48k 版为了换精确 48k 用过 16bit 帧，代价是小声压时有效位不足 |
| D | 多采样率（48k + 96k） | 母工程的定位就是模板，保留多档；平板专用的单档 48k 版在 LittleTail48k |
| E | `Ki = 0.0004`（积分时间常数约 5 秒） | 积分器较慢。实测在本工程这套水位/缓冲下可用；48k 版用 `Ki=0.004` 并把**预置积分项**作为欠载恢复手段 |
| F | 欠载恢复按"目标水位"整段填 0 | 每次欠载会插入一段静音。**本次把水位翻倍到 8ms 之后，这段静音也从 4ms 变成 8ms**（双刃剑：余量变大、但真欠载一次更明显）。48k 版加了 `gap` 参数只留 2ms |
| G | 流停 3ms 就 `memset` 整个环形缓冲 | 会丢弃缓冲中已有的有效音频，且在 TIM2 中断里阻塞 USB。48k 版改为只清"播放位置之后一小段" |
| H | `bInterval = 1`（每微帧一包，8000 URB/s） | 主机侧提交压力较大。48k 版用 `bInterval=4`（1ms 一包）降到 1000 URB/s |
| I | 保留 CDC 调试打印 | 本工程刻意保留：50ms 遥测行 + `p=`/`i=`/`d=` 在线调参。48k 版因平板打不开串口而整体编译掉 |

> 每一条的完整推导、实测数据和取舍过程，都写在 48k 版的 README 与代码注释里。

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
