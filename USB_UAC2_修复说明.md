# USB UAC2 声卡：枚举修复 + ES9018K2M 完整适配

芯片 CH32V305FBP6 / USBHS 高速 / CherryUSB / 复合设备（CDC + UAC2 声卡）

---

## 0. 现在的信号链

```
PC ──USB(UAC2, 异步 iso + 反馈端点)──> CH32V305
                                        │  USB 中断：拆半字写环形缓冲
                                        │  DMA1_CH5 循环搬运
                                        └──I2S2(PB12 WS / PB13 CK / PB15 SD,
                                                32bit 立体声, 主机)──> ES9018K2M
                                                                          └── 耳机放大
主循环：Audio_Poll() —— 采样率切换 / 音量 / 静音的 I2C 操作
USB 中断：收包写缓冲、SOF 里测速并回反馈
```

- 采样率：24k / 48k / 96k（UAC2 离散点写法）
  （12k 需要 MCLK/BCLK = 32，芯片只给 4/8/16，24.576M 晶振做不出来，见 8.7）
- 格式：32bit 立体声，I2S Philips 标准，MCU 作主机，ES9018 作从机
- MCLK：ES9018 用板上的 24.576M 晶振（沿用你原来的接法），MCU 不输出 MCK

---

## 1. 代码 10 的直接原因：三个描述符缺陷

原来 `conf_desc[]` 的长度是自洽的（210 = `0x00D2`），所以问题不在长度，而在字段语义：

| # | 位置 | 原来 | 应该 | 后果 |
|---|------|------|------|------|
| 1 | Clock Source `bmControls` | `0x00` | `0x03` | UAC2 里采样率**只能**通过 Clock 实体读写；写 0 等于告诉主机"我没有采样率控制"，主机读不到也设不了任何采样率 |
| 2 | Feature Unit `bLength` | `0x0A`(10) | `0x12`(18) | UAC2 的 FU 没有声道数字段，声道数由 `bLength = 6+(n+1)*4` 反推；10 字节被解析成 **0 声道**，与 IT 的 2 声道自相矛盾 |
| 3 | AS 接口 `bTerminalLink` | `0x04`(扬声器输出终端) | `0x02`(USB Streaming 输入终端) | UAC 里 AS 接口连的是**紧邻 USB** 的终端；播放用 Input Terminal，只有录音才连 Output Terminal |

第 1 条的注释和代码是反的——`0x03` 被写进了 `bmAttributes`，`bmControls` 留了 `0x00`。

另外把 AC Header 的 `bmControls` 从 `0x03` 改成 `0x00`：原来声明了 Latency Control
但工程里没实现，主机来读会 STALL，声明了却答不上来比不声明更糟。

### 修复前后

| | 修复前 | 修复后 |
|---|---|---|
| Feature Unit | 10 | 18 |
| AC `wTotalLength` | 56 (`0x0038`) | 64 (`0x0040`) |
| 配置 `wTotalLength` / 数组长度 | 210 (`0x00D2`) | 218 (`0x00DA`) |

现在所有长度由同一组表达式推导，并加了编译期兜底：

```c
#define AUDIO_FU_LEN        (6 + ((AUDIO_CHANNELS) + 1) * 4)
#define AUDIO_AC_ENTITY_LEN (8 + 17 + AUDIO_FU_LEN + 12)
#define AUDIO_AC_WTOTALLEN  (9 + AUDIO_AC_ENTITY_LEN)
#define CONFIG_DESC_LEN     (9 + CDC_ACM_DESCRIPTOR_LEN + \
                             AUDIO_V2_AC_DESCRIPTOR_LEN + AUDIO_AC_ENTITY_LEN + \
                             AUDIO_V2_AS_FEEDBACK_DESCRIPTOR_LEN)

_Static_assert(sizeof(conf_desc) == CONFIG_DESC_LEN,
               "conf_desc[] length does not match CONFIG_DESC_LEN");
```

---

## 2. USBHS 端口层的三个缺陷（CDC 用不到，声卡必须修）

> 沁恒 `CH32V307EVT_example\EVT\EXAM\USB\USBHS\DEVICE` 里**整套例程没有任何同步端点**，
> 这部分没有官方样例可抄，只能按寄存器手册做。

### 2.1 同步端点的类型位从来没写过

`R32_UEP_TYPE` 里 bit n = `UEPn_T_TYPE`，bit n+16 = `UEPn_R_TYPE`，置 1 表示该方向是
同步(isochronous)传输。原代码这一段是空的：

```c
if (ep_type == USB_ENDPOINT_TYPE_ISOCHRONOUS) {
    /* ISO bit lives in ENDP_TYPE; CDC does not use it */
}
```

不写这一位，硬件会把 EP3 当批量端点，同步事务收发不了。现在按类型置/清该位，
并且同步端点固定用 DATA0 翻转（同步传输不使用 DATA 翻转序列，不能交给硬件自动翻转）。

### 2.2 `UEPn_MAX_LEN` 收发共用，导致端点索引冲突

CH32 的 `UEPn_MAX_LEN` **每个端点索引只有一份寄存器**，IN/OUT 共用。原来索引 3 同时被用于：

* `0x03` OUT 音频数据，最大包 1024
* `0x83` IN 反馈，最大包 4

打开顺序是 1024 → 4，后者覆盖前者，音频 OUT 端点的接收上限最后变成 **4 字节**，
96 kHz 下每微帧 96 字节的包会被截断。

反馈端点已改到 **`0x84`**（索引 4）：

| 索引 | 方向 | 用途 | 最大包 |
|---|---|---|---|
| 1 | IN | CDC 通知 `0x81` | 8 |
| 2 | OUT | CDC 数据 `0x02` | 512 |
| 2 | IN | CDC 数据 `0x82` | 512 |
| 3 | OUT | 音频数据 `0x03` | 1024 |
| 4 | IN | 反馈 `0x84` | 4 |

（索引 2 收发都是 512，长度相同所以没问题，这也是 CDC 一直正常的原因。）

### 2.3 SOF 上报路径

异步播放要靠反馈端点告诉主机"我这边实际消耗多快"，高速下每 125 µs 一个反馈包。
`ch32v30x_usb.h` 里两个位要分清楚：

* `UIS_TOKEN_SOF = 0x10`：**设备模式**下 SOF 以 token 形式出现在 TRANSFER 中断里
  （官方例程的 ISR 里就写着 `case USBFS_UIS_TOKEN_SOF: break;`）
* `UIF_HST_SOF = 0x08`：**主机模式**的 SOF 定时器标志

所以上报放在 TRANSFER 分支里，dispatch 到 CherryUSB 已有的 `usbd_event_sof_handler()`。

---

## 3. 音频数据通路

### 3.1 字节序：为什么必须逐样点拆半字

USB 送来的 32bit 立体声是小端字节序：

```
[L0 b0 b1 b2 b3][R0 b0 b1 b2 b3] ...
```

而 I2S2 的 `DATAR` 只有 16bit，32bit 槽位必须按"高半字在前"写两次（STM32F1/CH32 的 SPI
数据寄存器是 16bit），所以每个 32bit 样点在内存里要排成 `[hi16][lo16]`：

```
hi = (b3 << 8) | b2
lo = (b1 << 8) | b0
```

一个立体声帧 = 4 个半字。`Audio_RingWrite()` 在 USB 中断里完成这个转换。

### 3.2 环形缓冲 + 循环 DMA

```
audio_ring[4096]  (8 KB，半字)          DMA1_CH5 循环模式，一直在跑
        ▲                 ▲
     pos(读)          audio_wr(写)
        └── 待播数据 ─────┘
```

* 读指针由 `I2S2_DMA_Position()` 从 DMA 计数器反推（`总数 - 剩余`）
* 写指针比读指针领先 `audio_lead` 个半字（**2 ms** 的缓存量，按采样率换算）
* 可写空间 = `(pos - wr - 1) & MASK`，超了就丢尾并累加 `g_audio_overrun`
* 读指针追上写指针（`avail < 4` 半字）说明断流，把写指针重新拉开到目标缓存量并累加
  `g_audio_underrun`

> 缓存量决定延迟：2 ms 缓存量下，从 USB 到 I2S 的固有延迟就是几毫秒。

### 3.3 反馈值不是写死的，是测出来的

```
每个 SOF(125 µs)：读一次 DMA 位置，累加走过的半字数
每 64 个 SOF(8 ms)：fb = (累计半字 << 16) / (4 * 64)      // 16.16，单位=每微帧样点数
每个 SOF：把当前 fb 写进反馈端点（高速 bInterval=1，每个微帧都要有新数据）
```

这样测出来的是 **I2S 实际消耗速率**，MCU 晶振和主机晶振的偏差会被主机自动补偿掉
（数据流不会因为 ppm 偏差而周期性溢出/断流）。

反馈格式：高速 `bInterval = 1` 时是 **16.16 定点、单位"每微帧样点数"**，
48 kHz → 6.0 (`0x00060000`)，96 kHz → 12.0 (`0x000C0000`)。
顺带修了 CherryUSB `AUDIO_FEEDBACK_TO_BUF_HS` 只写 3 字节的问题，这里补满 4 字节。

### 3.4 为什么 I2C 操作要放到主循环

`usbd_audio_open/close/set_volume/set_mute/set_sampling_freq` 全部由 CherryUSB 从
**USB 中断**里调用。而 ES9018 的寄存器读写走 80 kHz I2C，一次 mute 是
"读 0x07 + 写 0x07"两笔事务，约 0.3~0.5 ms。在 USB 中断里干这个会堵住 8 kHz 的
SOF 和其它端点，所以：

| 位置 | 做什么 |
|---|---|
| USB 中断 | 收包 → 拆半字 → 写环形缓冲；SOF → 测速 → 写反馈端点 |
| 主循环 `Audio_Poll()` | 开关流、改采样率、ES9018 静音/音量（I2C）、等锁定 |

中断里只置标志（`audio_want_stream` / `audio_rate_dirty` / 音量静音 dirty 位），
主循环按"期望状态"执行，天然幂等、也不会丢请求。

---

## 4. 采样率精度：为什么要把 I2S2 时钟源换成 PLL3

```
Fs = fI2SCLK / (64 * (2*I2SDIV + ODD))        32bit x 2ch = 每帧 64 个 BCLK
```

I2S2 的时钟源可选 `SYSCLK` 或 `PLL3_VCO`。本板 HSE = 8 MHz，实测两种情况：

| 时钟源 | 12000 | 24000 | 48000 | 96000 | 最差 |
|---|---|---|---|---|---|
| SYSCLK 144 MHz（原配置） | 11968 (-0.27%) | 23936 (-0.27%) | 47872 (-0.27%) | **97826 (+1.90%)** | 33 音分，明显走音 |
| **PLL3 80 MHz（现配置）** | 12019 (+0.16%) | 24038 (+0.16%) | 48077 (+0.16%) | **96154 (+0.16%)** | **2.8 音分，听不出来** |

80 MHz 下四个速率的分频比正好是 **104 / 52 / 26 / 13**，误差完全一致：
`PLL3 = HSE / PREDIV2(1) * PLL3MUL(10) = 8 MHz * 10 = 80 MHz`。

> 注意：`Peripheral/src/ch32v30x_spi.c` 的 `I2S_Init()` 是按
> `RCC_Clocks.SYSCLK_Frequency` 算分频的，切到 PLL3 之后它算出来的值是错的。
> 所以 `I2S2_Init()` 传 `I2S_AudioFreq_Default`，分频由 `I2S2_SetSampleRate()`
> 自己写 `I2SPR`。PLL3 没锁上时会自动退回 SYSCLK（96k 会有约 +1.9% 误差）。

`Clock Source` 的 `bmAttributes` 报 0，语义是"内部时钟、不同步于 SOF"，
因为数据端点是异步 + 反馈的结构。

---

## 5. ES9018K2M：手册核对 + MCU 主模式

> 你给的 `E:\PDF库\声卡技术文档\ESS-ES9018S-Datasheet-v2.3-NRND.pdf` 文件名写的是
> ES9018**S**，但正文标题和寄存器表都是 **ES9018K2M**（v3.7，2021-04-22），
> 正好是这颗芯片。

### 5.1 时钟约束（手册原文核对）

| 约束 | 出处 | 数值 |
|---|---|---|
| 每帧 BCLK 数 | p.10 格式表注 | 16bit=32 / 24bit=48 / **32bit=64** |
| FRAME 与 BCLK | Reg 0x0A 说明 | **FRAME = BCLK / 64** |
| 数据采样边沿 | p.30 时序注 | **DATA_CLK 上升沿**（tDS ≥ 4.1 ns，tDH ≥ 2 ns） |
| PCM 普通模式系统时钟下限 | p.7 | **MCLK > 192 × FSR** |
| 主模式 BCLK 分频 | Reg 0x0A [6:5] | **MCLK/4、/8、/16**（2'b11 也是 /16） |
| MCLK 上限 | p.30 Note 2 | 内部 DVDD 50 MHz；我们 24.576 MHz 余量很大 |

所以你说的那个"64 倍"精确表述是：**BCLK = 64 × FRAME_clk**（32bit × 2ch）。
XI 晶振就是 MCLK（芯片内部不再对 XI 分频），24.576 MHz 固定不变。

### 5.2 24.576 MHz 下的采样率核对

| Fs | BCLK = 64×Fs（MCU 出） | MCLK/BCLK | MCLK > 192×FSR | 芯片分频档 |
|---|---|---|---|---|
| 24 kHz | 1.536 MHz | 16 | ✓ | MCLK/16 (2'b10) |
| 48 kHz | 3.072 MHz | 8 | ✓ | MCLK/8 (2'b01) |
| 96 kHz | 6.144 MHz | 4 | 24.576M > 18.432M ✓ | MCLK/4 (2'b00) |
| 12 kHz | 0.768 MHz | 32 | ✓ | **没有这一档 → 不可用** |

**MCLK > 192×FSR 决定了 24.576 MHz 晶振下的 FSR 上限约 128 kHz**，
所以 96k 可以、192k 不行（192k 只能走 `MCLK = 128 × FSR` 的同步模式，
24.576M/128 正好等于 192k，那是另一种玩法）。
而 MCLK/BCLK 必须是 4/8/16 这一条把可用速率限制成 **24k/48k/96k 三个连续八度**。

这两条约束都已经写进代码：`ES9018_IsRateSupported()` 同时判两项，
`tools/uac2_audit/check_i2s_source.py` 会重算一遍并**交叉核对
`usb_app.c` 里对外报的每一档都有可用分频**，误加会直接 FAIL。

### 5.3 这次在 ES9018 驱动里改了三处

1. **软复位没有清 `soft_reset`**：原来写 `reg0 = temp | 0x01` 之后就没再管它，
   而手册写的是 `1'b1 复位芯片 / 1'b0 才是正常工作`。现在复位后补了一次
   `reg0 = 0x00`（osc_drv = full bias，手册推荐默认值）。如果芯片的复位位是自清的，
   这一写是空操作；如果不是，这就是"芯片一直停在复位态"的根因。
2. **`ES9018_Init()` 里的 Reg 0x0A 改成显式常量**，并写清了以后切到
   芯片主模式该填什么（见下）。当前值仍是默认的 `CLK_DIV_MCLK_4 | STOP_DIV_DEFAULT`，
   即 `master_clock_enable = 0`（芯片作从机）✓。
3. **补了 `ES9018_IsRateSupported()` 和时钟约束注释。**

> 你原来 header 里"Reg 7 bit[7] 必须写 0"的注释是错的：手册 Reg 7 的默认值是
> `0x80`，bit7 这个保留位要求为 **1**，所以 `0x80 | FILTER_SLOW_ROLLOFF` 是**对的**，
> 只是注释写反了，我把注释改了。

### 5.4 逐个寄存器对照结果（你写的值 vs 手册）

| Reg | 你写的值 | 手册默认 | 结论 |
|---|---|---|---|
| 0x00 | `temp\|0x01` | 0x00 | 复位位没清回 0 → 已修 |
| 0x01 | 0x80 | 0x8C | 32bit + I2S + 固定 I2S 输入 ✓ |
| 0x02 / 0x03 | 0x18 / 0x10 | 同 | 保留寄存器，值正确 ✓ |
| 0x04 | 0x00 | 0x00 | automute 关闭 ✓ |
| 0x05 | 0x68 | 0x68 | automute 门限 = 默认 ✓ |
| 0x06 | 0x4A | 0x4A | ✓ |
| 0x07 | 0xA0 | 0x80 | bit7=1 保留位默认值 ✓，filter = slow rolloff ✓ |
| 0x08 | 0x10 | 0x10 | ✓ |
| 0x09 | 0x22 | 0x22 | 保留寄存器，写的是手册标称默认值 ✓ |
| 0x0A | 0x05 | 0x05 | 从模式 ✓（改成显式常量） |
| 0x0B | 0x02 | 0x02 | L/R 不交换 ✓ |
| 0x0C | 0x5A | 0x5A | DPLL 带宽默认 ✓ |
| 0x0D | 0x00 | 0x40 | 开 THD 补偿但系数为 0 → 手册说这正是"PCM/DSD 增益一致"的推荐写法 ✓ |
| 0x0E | 0xCA | 0x8A | soft_start_on_lock = 1（你加的）✓ |
| 0x0F/0x10 | 0x00 | 0x00 | 0 dB ✓ |
| 0x11–0x14 | FF FF FF 7F | 0x7FFFFFFF | master_trim 默认值 ✓ |
| 0x15 | 0x00 | 0x00 | ✓ |

结论：**除了软复位那一位，你写的寄存器值全部正确**（要么等于默认，要么是手册明确
推荐的用法）。

### 5.5 驱动侧新增的功能

* **静音**：`ES9018_Mute(ES9018_MUTE_BOTH)` / `ES9018_Mute(0)`（reg 0x07 bit1:0）。
  开关流和换采样率前后都会静音，避免爆音。
* **音量**：`ES9018_DbToVolume()`，把 USB 音量（-100..0 dB）映射到衰减寄存器
  （手册确认 0.5 dB/步、0 = 0 dB）→ `vol = -dB × 2`，范围 0..200。
  默认 -20 dB（和你原来 `ES9018_SetVolume(40)` 一致）。
* **等锁定**：`ES9018_WaitLock()` 轮询 `reg 0x40` bit0。锁定时间由 `stop_div`
  决定：默认 5 = 2730 个 FSR 边沿 → 96 kHz 时约 14 ms，12 kHz 时约 110 ms，
  所以超时给了 200 ms。超时也不影响出声，`reg 0x0E` 的 `soft_start_on_lock`
  会让芯片自己保持静音。

### 5.6 后面要做的"MCU 作从模式"（芯片主模式）

手册允许芯片驱动 BCLK/LRCK，但**只在 32-bit I2S 下**（你的 `reg 0x01 = 0x80`
正好满足）。那时 MCU 的 I2S2 要从 `I2S_Mode_MasterTx` 改成 `I2S_Mode_SlaveTx`，
PB13/PB12 改成输入（`GPIO_Mode_IN_FLOATING`），并且要接上芯片的
`DATA_CLK`（BCLK）和 `DATA1`（FRAME）。

芯片主模式下就是用它自己的 24.576 MHz 直接分频，**完全同步、精度 0 ppm**：

| Reg 0x0A 配置 | FRAME |
|---|---|
| `MASTER_CLK_EN \| CLK_DIV_MCLK_4` | 96 kHz |
| `MASTER_CLK_EN \| CLK_DIV_MCLK_8` | 48 kHz |
| `MASTER_CLK_EN \| CLK_DIV_MCLK_16` | 24 kHz |
| 12 kHz | 需要 MCLK/32，芯片没有这一档 → **主模式下做不到** |

也就是说切到从模式后，12 kHz 这一个速率会丢失（除非换晶振）。代码里
`ES9018_Init()` 的 Reg 0x0A 处已经把三种填法写在注释里，换一行就行。

> 顺带说明为什么现在这种"MCU 主模式"仍然能用：`reg 0x0C` 的 DPLL/ASRC 没关
> （默认带宽 0101），芯片用自己的 24.576 MHz 做 DSP 时钟、用 DPLL/ASRC 去跟
> MCU 送来的 fs，所以 MCU 那 ±0.16% 只是让 ASRC 多修一点，不走音。
> 而切到芯片主模式后两边时钟同源，连 ASRC 都不需要了 —— 这也是它值得试的原因。
> （这一段是从手册的 DPLL/ASRC + `sync_mode` 描述推出来的，不是手册原话。）

### 5.7 I2S 位时钟极性：这次改了 `I2S_CPOL_High` → `I2S_CPOL_Low`

手册 p.30 写得很明确：

> "Audio data on DATA[2:1] are sampled at the **rising edges** of DATA_CLK"

标准 I2S（Philips）就是位时钟空闲为低、数据在下降沿变化、接收端上升沿采样，
对应 `I2S_CPOL_Low`。原来的 `I2S_CPOL_High` 会把位时钟反相，接收端的
"上升沿"正好落在数据跳变的那一瞬间，会读到错位的数据。

> 这个值是照抄沁恒 `EVT\EXAM\I2S\I2S_DMA` 例程的，但那个例程是 SPI2 主发 →
> SPI3 从收的**自回环**，两边极性自洽，所以它用 High 也能"通"，
> 不能作为接外部 DAC 的依据。
>
> 这是一行改回去就能试的事：如果换过来反而不出声/出噪声，把
> `Syscfg.c` 里的 `I2S_CPOL_Low` 改回 `I2S_CPOL_High` 即可（但按手册应该用 Low）。

---

## 6. 验证方法与当前结果

```powershell
& tools\uac2_audit\verify.ps1
```

它做四件事：重新编译改动过的源文件 → 和 `obj/` 里已有的 `.o` 一起链接出完整 ELF
→ 从 ELF 里读描述符**真实字节**做 UAC2 规则校验 → 交叉核对 I2S 分频配置。

当前结果（全部通过）：

```
text 40580   data 1680   bss 13204        RAM 共 14884 / 32768 字节 (45%)

descriptor audit:
  config: wTotalLength=218 (array=218), bNumInterfaces=4
  CLOCK id=1 bmAttributes=0x00 bmControls=0x03
  FU    id=3 bLength=18 -> 3 bmaControls -> 2 channel(s)
  AS if 3 alt 1 bTerminalLink=2 -> correctly links to Input Terminal 2
  feedback EP 0x84 mps=4 bInterval=1
  errors=0 warnings=0

I2S rate check:
  PREDIV2 = 1, PLL3MUL = 10 -> PLL3 VCO = 80 MHz   (与 I2S2_PLL3_HZ 一致)
  12k/24k/48k/96k 全部 +0.1603%  (2.77 音分)
```

另外用反汇编核对过（注意 CH32 的 `xw` 扩展必须带 `-M xw`，否则 objdump 会把
指令解错、甚至错位）：

* `usbd_ep_open` 里确实对 `ENDP_CONFIG`(+16) 和 `ENDP_TYPE`(+20) 做了读改写
* `USBD_IRQHandler` 里确实调用了 `usbd_event_sof_handler`
* `I2S2_SetSampleRate` 里 `sh a4,32(a5)` 写的正是 `SPI2->I2SPR` (0x40003820)

> `obj/` 里的 `LittleTail.elf / .hex / .lst / .map` 已经用工程自己的 makefile
> 重新构建过（`make main-build`，text 40616 / data 1680 / bss 13204），
> 可以直接烧 `obj\LittleTail.hex`。在 MounRiver Studio 里重新 Build 一次也一样。
>
> 小坑：在 `obj/` 里直接敲 `make` 只会编译第一个目标（默认目标被包含进来的
> `.d` 依赖文件抢先定义了），要显式写 `make main-build`。

---

## 7. 上板验证清单

1. MounRiver Studio 重新 Build、烧录。
2. 设备管理器：声音设备下出现 LittleTail，**不再有感叹号**。
   CDC 因为 PID 变了会是一个新设备实例，COM 口可能变号。
3. 播放音频，按顺序确认：
   * 示波器看 **PC9**：应在 `Audio_in_callback` 里翻动（说明包在进）
   * 示波器看 **PB13(BCK)**：96k 时约 6.15 MHz；**PB12(WS)** 应约 96.15 kHz
   * 听声音有没有周期性"咔哒"（那是溢出/断流）
4. 如果怀疑数据流，把 `Audio_Poll()` 里加上打印
   `g_audio_packets / g_audio_overrun / g_audio_underrun / g_audio_actual_rate`
   （接上 CDC 后可用），正常情况 overrun/underrun 应该长期为 0 且不增长。
5. 在 Windows 声音设置里切采样率（或播放不同采样率的文件），确认 4 个速率都能锁上
   （`ES9018_WaitLock` 通过 = reg 0x40 bit0 为 1）。

---

## 8. 如果还是代码 10

**先取到真正的失败码**（最有价值的一步）：

* 设备管理器 → 该设备 → 属性 → 详细信息 → `Problem Status`（`DEVPKEY_Device_ProblemStatus`）
* 或命令行 `pnputil /enum-devices /problem`

| Problem Status | 方向 |
|---|---|
| `0xC00000E5` STATUS_DEVICE_DATA_ERROR | 描述符/拓扑一致性 |
| `0xC00000BB` STATUS_NOT_SUPPORTED | 某个类请求被 STALL |
| `0xC000009A` STATUS_INSUFFICIENT_RESOURCES | 先怀疑 Windows 更新回归，见下 |

**必须排除的干扰项**：2025 年 1 月的 Windows 更新让大量**原本正常**的 UAC2 DAC
报代码 10（"Insufficient system resources exist to complete the API."），
卸载 `KB5050021` / `KB5050009`(Win11) 或 `KB5049981`(Win10) 后恢复。

**Windows 缓存**：改了描述符但 PID 不变时可能继续用旧驱动实例；本次 PID 已从
`0x0000` 改成 `0x0001` 强制新实例，也可以手动"卸载设备 + 删除驱动程序"。

**Windows 版本**：CherryUSB 官方文档明确说 Win10 的 UAC2 支持不完整，
建议用 **Windows 11** 验证。

---

## 8.5 播放没声音：两个叠加的 bug（已修）

**现象**：USB 枚举正常、总线上抓得到音频包，但 DAC 完全没有输出，
三个 LED（PC7/PC8/PC9，低电平点亮）**全亮** = 三个引脚都是低 = 三件事一件都没发生。

### 根因 1（关键）：AudioStreaming 接口没有挂类回调

CherryUSB 分发 `USBD_EVENT_SET_INTERFACE` 的条件是（`usbd_core.c:1084`）：

```c
if (intf && intf->notify_handler && (desc->bInterfaceNumber == (intf->intf_num)))
    intf->notify_handler(busid, event, arg);
```

原来只给 **AudioControl 接口**挂了音频类回调：

```c
usbd_add_interface(0,usbd_audio_init_intf(0,&audio_intf_cmd,...));  // intf_num=2，有回调
usbd_add_interface(0,&audio_intf_data);                            // intf_num=3，没有回调
```

而主机切换备用设置时选中的是 **AudioStreaming 接口(3)**，于是这个事件被直接丢掉，
`usbd_audio_open()` / `usbd_audio_close()` **永远不会被调用**。

修法：两个接口都挂上（CherryUSB 的 UAC2 必须有 2 个接口，两个接口都要能收到事件）：

```c
usbd_add_interface(0,usbd_audio_init_intf(0,&audio_intf_cmd, 0x0200,audio_table,4)); // AC
usbd_add_interface(0,usbd_audio_init_intf(0,&audio_intf_data,0x0200,audio_table,4)); // AS
```

### 根因 2：音频 OUT 端点从来没有被"挂上接收"

`usbd_ep_start_read(EP_AUDIO_OUT, ...)` 原来**只写在 `Audio_in_callback()` 里**
（收完一包再挂下一包）——典型的先有鸡还是先有蛋：第一次接收没人挂，
回调就永远不触发，也就永远没人挂第二次。主机看到端点一直 NAK，一个字节都进不来。

修法：在 `Audio_StreamPrepare()`（主循环，选好备用设置之后）挂第一包，
`Audio_in_callback()` 继续负责后续每一包。

> 这两条合起来正好解释了"抓包有音频包 + 设备一个字节没收到"：
> 那些包在总线上是被 NAK 的，抓包工具照样会显示出来。

### 顺带修掉的两个隐患

* `g_audio_packets` 原来在回调开头无条件自增，缓冲还没准备好也会自增，
  会导致"用空缓冲启动 I2S"。现在改由 `Audio_RingWrite()` 在真正写进缓冲时才自增。
* `audio_table` 里 `OUTPUT_TERMINAL` 的端点绑定从 `EP_AUDIO_OUT` 改成
  `EP_AUDIO_FEEDBACK`（CherryUSB 的实体查找是顺序匹配的，语义要对上）。

### LED 指示改成"稳定亮灭"（按你的硬件）

PC7/PC8/PC9 是**低电平点亮**的三个 LED，而且看不到高频闪烁，
所以原来那套 1kHz/500Hz 方波没用，现在改成每 200ms 刷新一次的稳定状态：

| LED | 亮 = | 灭 = |
|---|---|---|
| **PC9** | 最近 200ms 内有音频包写进缓冲 | 没有数据进来 |
| **PC8** | I2S 数据流已启动 | 流没启动 |
| **PC7** | ES9018 `LOCK_STATUS` = 1（时钟已锁） | 没锁 / 流没启动 |

**上电时三个灯全灭**（`LED_Init` 里置高），所以任何一盏亮起来都说明那一步通了。
正常播放时应该**三个灯都亮**。

时间基优先用 SOF（8 kHz，`g_audio_sof_ticks`），SOF 不来时退回循环计数兜底，
保证 USB 没配置好也能刷新。PC7 只在流启动后才读 I2C（一次约 0.3 ms）。

### 这张表能一次定位

| PC9 | PC8 | PC7 | 结论 |
|---|---|---|---|
| 灭 | 灭 | 灭 | USB 音频包没进来（备用设置没切过来 / 端点没挂上） |
| 亮 | 灭 | 灭 | 包进来了但主循环没启动流 |
| 亮 | 亮 | 灭 | **I2S 在发但 ES9018 锁不上 → 查 I2S 极性/接线/采样率** |
| 亮 | 亮 | 亮 | 时钟已锁 → 问题在数据内容 / 静音 / 模拟输出侧 |

---

## 8.6 波形不连续：预存 + 反馈（已修）

有波形之后剩下的两个问题——**开头/周期性不连续**，和**缓冲区被时钟误差慢慢灌满或抽干**。

### 8.6.1 反馈值原来根本没送到主机（这是"不连续"的主因）

原来的反馈只写在 `Audio_OnSof()` 里，而 `Audio_OnSof()` 只由 `USBD_EVENT_SOF` 驱动：

```c
case USBD_EVENT_SOF:
    if (audio_ring_on) {
        Audio_OnSof(busid);       /* 反馈只在这里发 */
    }
```

也就是说**反馈能不能发出去，完全取决于 SOF 事件有没有上报**。CH32 的 SOF 上报路径
（token 还是 `UIF_HST_SOF`）我们只能从寄存器定义推断，没上板确认过；
一旦收不到，主机就永远拿不到反馈，只能按**标称速率**发数据。

后果可以精确算出来：

```
I2S 实际采样率   = 48076.9 Hz（PLL3 80MHz ÷ 26 ÷ 64）
主机按标称发送   = 48000   Hz
差值             = +76.9 帧/秒 = +308 半字/秒
预存量 5ms      = 240 半字 @48k
=> 240 / 308 ≈ 0.78 秒断流一次
```

这正好就是"经常出现不连续"的周期量级。

**改法：反馈不再依赖 SOF。**

1. **反馈值由已知的 I2S 时钟直接算出**（`I2S2_GetFeedbackHS()`）：

   ```
   fb = fI2SCLK * 65536 / (64 * D * 8000)        16.16 定点，单位=每微帧样点数
   ```

   这比"DMA 走了多少"的测量法更准：I2S 的采样率完全由 PLL3/HSE 决定，
   我们精确知道它是多少；测量法还要依赖 SOF 时间基，而那个恰恰不确定。

2. **持续续包**：`Audio_StreamKick()` 里主动发第一包，
   之后每次发完在 `Audio_feedback_callback()` 里立刻挂下一包——
   只要主机来 poll，端点里永远有数据，**不需要 SOF、也不需要定时器**。

四个速率对应发给主机的反馈值（已由校验脚本算出并核对）：

| Fs | fb (16.16) | 十六进制 | 标称值 | 差值 |
|---|---|---|---|---|
| 24 kHz | 196923 | `0x0003013B` | 196608 | +0.16% |
| 48 kHz | 393846 | `0x00060276` | 393216 | +0.16% |
| 96 kHz | 787692 | `0x000C04EC` | 786432 | +0.16% |

差的那 0.16% 正是 PLL3 分频能做到的最接近值——主机收到这个值就会
按 **48.077 kHz** 而不是 48.000 kHz 发数据，两边速率对齐，缓冲区不再漂移。

> 残余误差只剩 HSE 晶振本身的 ppm（两个晶振差几十 ppm 量级），
> 相对 0.16% 小了约 100 倍，换算下来泄漏 5ms 缓冲需要几分钟。
> 如果还想更稳，下一步可以做"按缓冲区水位微调反馈值 ±1 LSB"
> （1 LSB = 1/65536 = 15 ppm，粒度刚好合适）。

### 8.6.2 开 I2S 之前先预存数据（你提的第一条）

原来写指针一上来就摆在"读指针 + 2ms"处，缓冲里那 2ms 全是 0，
DMA 一开就先放 2ms 静音，而且只攒了**一包**数据就开时钟。

现在改成：

```
Audio_StreamPrepare()  缓冲清 0、配分频、装 DMA、挂端点；写指针 = 0，不开 I2S
（主机开始送包，数据从索引 0 往后堆）
Audio_Poll()           攒够 AUDIO_PREFILL_MS = 5ms 才 Audio_StreamKick()
Audio_StreamKick()     静音 -> I2S2_DMA_Start()（DMA 从 0 开始）
                       -> audio_lead = audio_wr（实际攒到的量就是缓存量）
                       -> 发第一包反馈 -> 解除静音
```

好处：DMA 一开就**紧接着第一包真实数据**，既没有开头静音段，
也不会出现"才一包数据就开时钟、DMA 立刻追上去"的情况。
5 ms 预存量也就是多 5 ms 延迟。

### 8.6.3 断流时补静音，而不是重放旧音频

原来断流的处理是把写指针往前跳一个"缓存量"：

```c
audio_wr = (pos + audio_lead) & AUDIO_RING_MASK;   /* 旧做法 */
```

这一跳等于把环形缓冲里**上一圈的旧音频**重新放一遍（大约几毫秒），
听起来就是一个很明显的咔哒。现在改成在写指针前面补一小段**静音**
再把写指针推过去：

```c
if ((avail < AUDIO_MIN_AVAIL) && audio_stream_on) {
    gap = min(audio_lead - avail, AUDIO_SILENCE_MAX);
    把 [audio_wr, audio_wr+gap) 清零;
    audio_wr += gap;
    g_audio_underrun++;          /* 计数可以观察频率 */
}
```

`AUDIO_SILENCE_MAX = 256` 是为了限制在 SOF 中断里的循环长度
（约 2 µs，不会影响 8 kHz 的节奏）。这样断流只会听到一小段静音，
而不是旧数据重放。

### 8.6.4 怎么确认反馈生效了

反馈是否真的在发，看 `g_audio_fb_sent`（每次成功挂包 +1）。
正常播放时它应该以微帧速率增长（8 kHz 量级，实际受主机 poll 节奏影响）。
接上 CDC 打印一下就能看到；`g_audio_fb_value` 是当前值，
应该等于上表里对应的那个数。

如果 `g_audio_fb_sent` 一直是 0，说明反馈端点没被主机 poll 到
（检查 `bInterval = 1`、`wMaxPacketSize = 4`、端点 `0x84`）。

---

## 8.7 采样率差 2 倍：MCLK 分频必须跟着采样率走（已修）

**现象**：播放 2 kHz 正弦，示波器上出来的是 4 kHz；同时波形持续不连续。

### 根因

`ES9018_Init()` 里把 Reg0x0A 的 `clock_divider_select` **写死成了 /4**：

```c
ES9018_WriteReg(ES9018_REG_MASTER_MODE_CTRL,
                ES9018_CLK_DIV_MCLK_4 | ES9018_STOP_DIV_DEFAULT);   /* 旧代码：永远 /4 */
```

但这个比值是 **MCLK(24.576M) 与 BCLK 的比**，而 BCLK = 64 × Fs，所以它必须跟着采样率变：

```
比值 = 24.576MHz / (64 x Fs) = 384000 / Fs

   Fs = 96 kHz -> 比值 4   -> CLK_DIV_MCLK_4   (2'b00)
   Fs = 48 kHz -> 比值 8   -> CLK_DIV_MCLK_8   (2'b01)   <-- 我们一直写成 /4
   Fs = 24 kHz -> 比值 16  -> CLK_DIV_MCLK_16  (2'b10)
   Fs = 12 kHz -> 比值 32  -> 芯片没有这一档
```

48 kHz 下写成 /4，比值就只有正确值的一半，芯片按 **2 倍速率**转换
→ 2 kHz 变 4 kHz，**同时缓冲区以 2 倍速度被抽干**，所以波形持续不连续。
两个现象是同一个原因。

### 改法

新增 `ES9018_SetClockDivider(fs)`，按采样率选 4/8/16：

```c
case 96000: return ES9018_CLK_DIV_MCLK_4;
case 48000: return ES9018_CLK_DIV_MCLK_8;
case 24000: return ES9018_CLK_DIV_MCLK_16;
default:    return ES9018_CLK_DIV_INVALID;     /* 12k：比值 32 不存在 */
```

调用点：
* `ES9018_Init()` 里按默认 48 kHz 设一次；
* `Audio_StreamPrepare()` 里**每次开流/换采样率都重设**（I2C 操作，主循环上下文）。

`master_clock_enable` 仍然 = 0（MCU 作 I2S 主机、芯片作从机），
只是把"XI 比 BCLK 快多少倍"这个信息告诉芯片。

### 连带结论：12 kHz 用 24.576M 晶振做不出来

12 kHz 需要的比值是 32，而芯片只提供 4/8/16 三档。
24.576 MHz 晶振下能覆盖的正好是 **24k / 48k / 96k** 三个连续八度。

所以已经把 **12 kHz 从 UAC2 采样率表里去掉**了（`AUDIO_FREQ_COUNT` 3，
表里只剩 24k/48k/96k）。理由：报给主机但选中后输出 2 倍速率，
比不报更糟。校验脚本会交叉核对"表里报的每一档都必须有可用分频"，
以后误加会直接 FAIL。

想要 12 kHz 的话需要换晶振：`0.768 MHz x {4,8,16} = {3.072, 6.144, 12.288} MHz`
—— 比如 12.288 MHz 晶振可以覆盖 12k/24k/48k，但就做不了 96k 了。

### 验证

```
Reg 0x0A clock_divider_select  (MCLK/BCLK must be 4, 8 or 16):
  Fs       BCLK=64*Fs   needed ratio   divider
   24000      1.536 MHz          16   MCLK/16   2'b10
   48000      3.072 MHz           8   MCLK/8    2'b01
   96000      6.144 MHz           4   MCLK/4    2'b00
advertised rates in usb_app.c: [24000, 48000, 96000]
RESULT: PASS
```

烧进去后 2 kHz 应该就是 2 kHz 了；如果还是 2 倍，说明这个比值的影响路径
和我的推断不同（那就先试 /4 和 /16 两个值，各试一次就能定位）。

---

## 8.8 I2S 实际速率可读：CDC 每秒打印一行状态

采样率差 2 倍这件事，光看代码没法定位（可能是主机选了 96k、可能是我们假定的
I2S 时钟不对、也可能是数据格式 CHLEN 没配成 32bit），所以加了直接读出来的手段。

### 8.8.1 先澄清一个容易踩的点

`I2S2_Init()` 末尾那行：

```c
(void)I2S2_SetSampleRate(48000u);     /* 这一行是无效的 */
```

它只是开机时给个初值。**真正生效的是 `Audio_StreamPrepare()` 里的这一行**：

```c
g_audio_actual_rate = I2S2_SetSampleRate(g_audio_freq);   /* g_audio_freq 来自主机 SET_CUR */
```

所以改 `I2S2_Init()` 里那个 48000 **本来就不会有任何变化** —— 不是函数坏了，
是它立刻被后面那次调用覆盖了。要改采样率应该改主机侧设置，或者改
`g_audio_freq` 的初值。

### 8.8.2 每秒一行状态（打开 COM 口就能看）

`printf` 现在真的能用了：`Debug/debug.c` 里早就有 `_write → CDC_WriteBlocking`，
但 `CDC_WriteBlocking` 是 **`while(tx_busy);` 无界等待** ——
主机不开 COM 口时端点不会完成，`printf` 会把主循环直接卡死。
现在改成"分块 + 每次最多等约 2ms，超时丢弃"，绝不会阻塞音频。

`Audio_DiagPrint()` 每秒打一行：

```
fs=48000 i2spr=0x00D div=26 clk=80000000 meas=48077 cfgr=0x0E05 en=1 pkt=... ovr=0 unr=0 fb=393846 lock=1
```

| 字段 | 含义 | 48 kHz 时应该是 |
|---|---|---|
| `fs` | 主机 SET_CUR 选中的采样率（真正生效的那个） | 48000 |
| `i2spr` | `SPI2->I2SPR` 原值（I2SDIV + ODD） | `0x00D`（I2SDIV=13, ODD=0） |
| `div` | `2*I2SDIV+ODD` | 26 |
| `clk` | 我们**假定**的 I2S 时钟输入频率 | 80000000 |
| `meas` | **用 USB SOF(8kHz) 当基准实测**的 I2S 帧率 | ≈48077 |
| `cfgr` | `SPI2->I2SCFGR`（I2SMOD/I2SE/CHLEN/DATLEN 全在里面） | `0x0E05` |
| `en` | I2SE | 1 |

`cfgr = 0x0E05` 拆开看：I2SMOD(bit11)=1、I2SE(bit10)=1、I2SCFG(9:8)=10(主机发送)、
CKPOL(bit3)=0(空闲低)、I2SSTD(5:4)=00(Philips)、**DATLEN(2:1)=10(32bit)**、
**CHLEN(bit0)=1(32bit 通道)**。这三项决定了"每帧 64 个 BCLK"。

`meas` 是这次的关键：它把 I2S 的实际速率和主机的 8 kHz 基准直接比出来，
不用示波器也能判断。

### 8.8.3 怎么按这行字定位

| 现象 | 结论 | 下一步 |
|---|---|---|
| `fs=96000` | 主机自己选了 96 kHz | 那 96k 输出反而是对的；在 Windows 声音设置里把格式改成 48k 再看 |
| `fs=48000` 但 `meas≈96154` | 我们假定的 80 MHz 时钟不是真的（实际约 160 MHz），或 CHLEN 不是 32bit | 看 `cfgr` 是不是 `0x0E05`；如果是，就把 `Syscfg.c` 里 `I2S2_PLL3_HZ` 改成 `160000000u` 试一次（`div` 会翻倍） |
| `meas=0` | SOF 事件没上报 | 反馈已经不受影响（走端点续包），但测量要换基准 |
| `cfgr` 的 bit0 = 0 或 bits2:1 ≠ 10 | 数据格式没配成 32bit | 改 `I2S_DataFormat_32b` 那组配置 |

---

## 9. 已知限制 / 后续可改进

3. **反馈值目前是"由 I2S 时钟精确算出"的**，精度只受 HSE 晶振 ppm 限制。
   想做ppm级闭环，可以在水位偏高中/偏低时把反馈值 ±1 LSB（1 LSB = 15 ppm）
   做慢速积分。这属于优化，不是必需。
2. **没有专门的抖动缓冲策略**：环形缓冲的缓存量靠初值 + 反馈维持。
   如果主机送数据的节奏抖动很大，可能出现偶尔的 overrun/underrun 计数增长。
3. **`ES9018_WriteReg` / `ReadReg` 里的 I2C 等待是无界循环**（原来的代码就是这样），
   如果 ES9018 不应答会卡死主循环。建议后续加超时。
4. **只做了播放（主机→设备）**，没有录音路径，描述符里也没有 AS 输入接口。
5. **音量映射假设 0.5 dB/步**（来自你 header 里的注释），实测不符只改
   `ES9018_DbToVolume()` 一个函数即可。
6. **`TIM1_Init()`** 仍然是初始化了但没被用到（原来的代码），我没动。

---

## 10. 改动文件清单

| 文件 | 改动 |
|---|---|
| `User/usbd_desc.h` | UAC2 描述符全部修正 + 长度推导 + 编译期断言；反馈端点改 `0x84`；PID 改 `0x0001` |
| `User/usb_app.c` | 采样率/音量状态、4 个离散采样率、环形缓冲 + 半字转换、实测反馈、`Audio_Poll()` 状态机（准备/启动分离）、`Audio_DiagPoll()` 诊断、open/close |
| `User/usb_app.h` | 新增 `Audio_Poll()` 与诊断计数声明 |
| `User/Syscfg.c` / `.h` | 新增 `I2S2_ClockInit()`（PLL3 80 MHz）、`I2S2_SetSampleRate()`、`I2S2_DMA_Position()`；`I2S2_Init()` 不再让库算分频；**I2S 极性改成 `I2S_CPOL_Low`** |
| `User/ES9018.c` / `.h` | 软复位后清 `soft_reset`；Reg 0x0A 改显式常量并写明主模式填法；新增 `ES9018_WaitLock()`、`ES9018_DbToVolume()`、`ES9018_IsRateSupported()`；补时钟约束注释；修正 Reg 7 保留位注释 |
| `User/main.c` | 去掉本地正弦表和阻塞 printf，主循环只跑 `Audio_Poll()` |
| `Cherry_USB/port/usbhs/usb_dc_usbhs.c` | 同步端点类型位、同步用 DATA0 翻转、SOF token 上报 |
| `Cherry_USB/port/usbhs/usb_usbhs_reg.h` | 新增 `ENDP_T_TYPE_BIT` / `ENDP_R_TYPE_BIT` |
| `tools/uac2_audit/` | 校验工具（新增）：描述符校验、I2S 分频交叉核对、一键验证脚本 |

---

## 9. Audio OUT 回调"只进一次"的真正原因：USBHS 的 UIF_ISO_ACT

### 9.1 现象
- `g_audio_out_irq`（回调计数）= 1，怎么都不涨；
- USB 抓包看到主机一直在正常发 ISO OUT 包（数据是进去的）；
- 描述符、缓冲、反馈端点全部修好之后依然如此。

### 9.2 原因
CH32V307 有**两个 USB 控制器**，它们的传输完成中断不一样：

| | 传输完成标志 | 同步(ISO)完成标志 |
|---|---|---|
| USBFS（全速） | `UIF_TRANSFER (0x02)` | **没有这一位** |
| USBHS（高速） | `UIF_TRANSFER (0x02)` | `UIF_ISO_ACT (0x40)` |

`ch32v30x_usb.h` 里 USBHS 独有：

```c
#define USBHS_UIE_ISO_ACT   0x40   /* INT_EN  */
#define USBHS_UIF_ISO_ACT   0x40   /* INT_FG  */
```

而 USBFS 的 `USBFS_UIE_* / USBFS_UIF_*` 里根本没有 ISO_ACT。所以网上/EVT 里那些
USBFS 的 UAC 例程（`EVT/EXAM/USB/USBFS/DEVICE/UAC10_Headphone`）都不需要处理它 ——
**照搬它们的思路写 USBHS 端口就会踩这个坑**。

原来的 `USBD_IRQHandler` 三个地方一起出错：

1. 只判断 `UIF_TRANSFER`；
2. 只在那一个分支里清 `UIF_TRANSFER`；
3. `INT_EN` 里没有使能 `UIE_ISO_ACT`（= 中断是关着的）。

于是同步端点的第一次完成（同时置了 TRANSFER）被当成普通传输处理掉了 —— **所以恰好能
进去一次**；它顺手置起来的 `ISO_ACT` 没人清、中断又关着，中断服务程序根本没机会清；
之后同步端点的完成只走 `ISO_ACT` 这一路，就再也进不了中断了。

**关键点**：包是 SIE/DMA 收下的（主机那边看一切正常），死掉的只是"完成中断"这一路，
所以表现为"抓包正常 + 回调只进一次"，而不是 NAK 或者设备掉线。

### 9.3 修法（`Cherry_USB/port/usbhs/usb_dc_usbhs.c`）
1. `INT_EN` 加上 `USBHS_UIE_ISO_ACT`，让 ISO 完成能进中断；
2. 把 `UIF_TRANSFER | UIF_ISO_ACT` 当成同一次传输完成：一次中断只分发一次，
   两个标志一起清；
3. 同步 OUT 端点不再用 `TOG_OK` 过滤（ISO 的数据 PID 由主机固定为 DATA0，
   这一位在同步端点上没有意义，一旦为 0 就会把包 ACK 掉却不上交）。

### 9.4 诊断手段
端口里留了一组临时计数器（`g_dbg_*`），加到 MounRiver 的 Watch 窗口看
（**千万不要下断点** —— 断点会把 CPU 停住、端点停在 NAK，主机会直接丢掉整个流，
看到的次数是假的）：

| 变量 | 含义 |
|---|---|
| `g_dbg_irq` | USB 中断进入次数（中断还活着没有） |
| `g_dbg_intst` | 最近一次 INT_ST 原值（令牌 + 端点 + TOG_OK） |
| `g_dbg_out3` | 主机发到 EP3 的 OUT 次数 |
| `g_dbg_iso3` | 其中被 UIF_ISO_ACT 上报的次数 |
| `g_dbg_togbad3` | 其中 TOG_OK=0 被丢掉的次数 |
| `g_dbg_cb3` | 真正进 handle_non_ep0_out 的次数（≈回调次数） |
| `g_dbg_rxctrl3` | 最近一次动完 EP3 后的 RX_CTRL 原值（低 2 位 = ACK/NAK） |
| `g_dbg_cfg3` | EP3 的 R_EN / R_TYPE 位（确认端点没被关掉） |

定位完这一块和 ISR 里的计数可以整体删掉。
