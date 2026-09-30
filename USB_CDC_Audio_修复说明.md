# CDC + Audio 复合设备：USBHS 端口层修复说明

适用：CH32V305FBP6 / USBHS 高速 / CherryUSB / CDC(虚拟串口) + UAC2(异步播放 + 反馈端点)

这次只动了两件东西，都在端口层，没有碰你的业务代码（ES9018 / I2S 环形缓冲 / 采样率切换）：

| 文件 | 改动 |
|---|---|
| `Cherry_USB/port/usbhs/usb_dc_usbhs.c` | 重写数据通路：同步端点、EP0、中断、总线复位 |
| `Cherry_USB/port/usbhs/usb_usbhs_reg.h` | 补同步端点的寄存器语义说明和 `USBHS_UIF_TRANSFER_MASK` |

---

## 1. 一句话结论

`CDC` 和 `Audio` 冲突的根源不在描述符，而在 **USBHS 端口层的同步(ISO)端点和 EP0 数据通路**；
按 `TinyUSB(src/portable/wch/dcd_ch32_usbhs.c)` 和沁恒量产 UAC2 固件
(`ch32v30x_usbhs_device.c`) 两边的共同做法重写后，两者可以同时工作。

---

## 2. 真正的根因（按影响排序）

### 2.1 同步端点的 DATA 翻转：写死 DATA0 和 TOG_AUTO 是互斥的，选错就"只进一次"

`R32_UEP_TYPE` 的类型位**只是告诉硬件"这是同步端点"**，翻转怎么走仍由
`UEPn_TX_CTRL/UEPn_RX_CTRL` 的 bit5(TOG_AUTO) 决定：

| 写法 | 结果 |
|---|---|
| 同步端点 + `TOG_AUTO` | 硬件按 DATA0/DATA1 交替期待 PID。同步 OUT 的事务主机**永远发 DATA0**，第二次开始 `TOG_OK=0` |
| 同步端点 + 固定 DATA0 | 每次都期待 DATA0，`TOG_OK` 恒为 1（正确） |

原来的 `USBD_IRQHandler` 里这一支：

```c
} else if (token == USBHS_UIS_TOKEN_OUT) {
    if (intst & USBHS_UIS_TOG_OK) { ...上交数据... }
    else { /* 只重新 ACK，不重挂 DMA */ }
}
```

`TOG_OK=0` 时包被 ACK 掉、数据不上交，而且**不重新挂 DMA**，于是从第二包起每一包都从这里漏掉
—— 表现就是"抓包看主机一直在发，回调只进一次"。这跟"设备掉线"完全不同，很容易误判成
端点没挂上。

**现在的做法**：同步端点固定 DATA0、不带 TOG_AUTO；同步 OUT **完全不看 `TOG_OK`**
（同步端点丢一包就永远补不回来，宁可收下也不丢）；非同步端点照旧看 `TOG_OK` 并走
`TOG_AUTO` 自动翻转（CDC 一直就是这么工作的）。

### 2.2 同步 OUT 每包之间被 NAK 掉

原实现在中断里先把 `RX_CTRL` 写成 NAK，再等回调重新挂上。同步端点没有重传机制，
NAK 窗口里到达的包直接消失（回调里还会做 I2S 初始化，窗口可能远超一个微帧）。
现在同步 OUT **不 NAK**，只换 DMA 目标；非同步端点保持"先 NAK 再交给回调"的老做法。

### 2.3 `UIF_ISO_ACT`：不能"使能了不管"，也不能"只清不使能"

`ch32v30x_usb.h` 里 USBHS 比 USBFS 多一位 `UIF_ISO_ACT(0x40)`（USBFS 的 INT_FG 没有这一位，
所以网上那些 USBFS 的 UAC 例程都不管它，照抄就漏）。这两种错法都踩过：

| 做法 | 现象 |
|---|---|
| 使能 `UIE_ISO_ACT` 但中断里不清它 | INT_FG 常驻 0x40 → 中断反复重入 → CPU 占满 → "设备在、COM 口打不开" |
| 不使能、也不清 | INT_FG 里挂着 0x40 没人清，同步端点的完成事件可能退不掉 |

现在的做法：**默认不使能它的中断**（沁恒量产固件和 TinyUSB 都不使能），
但**每次进 USB 中断都写 1 清掉它**。为了让"另一种可能"也能一行切换，留了编译开关
`CH32_USBHS_ISO_ACT_INT`（见第 5 节）。

### 2.4 EP0：`UEP0_DMA` 被写成了 0 / 指向别处

原实现里 `usbd_ep_start_read(0, NULL, 0)`（控制传输的 0 长度状态阶段）会把
`UEP0_DMA` 写成 **0**；数据阶段又会把它指向 core 的 `req_data`。硬件收 SETUP 是
**先 DMA 后中断**，指针不在 setup 缓冲上时 SETUP 包就丢了 —— 复合设备要处理的控制请求
（SET_CONFIGURATION / SET_INTERFACE / 类请求）比单 CDC 多得多，这个隐患被放大。

现在的做法（和 TinyUSB 一样）：EP0 有自己的一块 512 字节 DMA 缓冲，
`UEP0_DMA` **整个生命周期只指向它，从不改动**；SETUP 包先抄一份副本再用，
IN 数据从 core 缓冲 memcpy 进来，OUT 数据收到了再 memcpy 回去。顺带修掉两个控制传输的经典坑：

* EP0 OUT 数据阶段每包要**翻转 DATA**（第 1 包 DATA1、第 2 包 DATA0…），原来没翻，
  `wLength > 64` 的控制下载会卡在第 2 包；
* 交给 core 的完成字节数必须是**本包收到的长度**，不是累计值（原来传累计值，
  `wLength > 64` 的控制下载会把缓冲指针推错）。

### 2.5 总线复位没有把端点拉回初始状态

CherryUSB 的 core 在复位时只重开 EP0，其余端点的 `ENDP_CONFIG/ENDP_TYPE` 它不管。
原来没清，等于把上一轮枚举的端点配置（包括同步类型位和挂着的 DMA）带进下一轮。
现在 `UIF_DETECT` 里调 `ch32_usbhs_hw_reset()`：所有端点回 NAK/DATA0、清类型位和包长、
EP0 重新就位等 SETUP，然后再交给 core 走 `usbd_event_reset_handler()`。

### 2.6 中断里"锁存顺序"

同步传输**不受 `UC_INT_BUSY` 约束**（硬件不会等软件清标志），所以下一包的完成随时可能
覆盖 `INT_ST/RX_LEN`。现在非 EP0 的处理顺序是：
先锁存 `INT_ST`→`RX_LEN` → 再清 `INT_FG` → 才跑用户回调。
EP0 例外（EP0 受 INT_BUSY 保护，提前放行反而会让新的阶段插进来）。

### 2.7 描述符是好的，不用改

`tools/uac2_audit/audit_uac2.py` 对着**编译后的 ELF** 跑了一遍：

```
config: wTotalLength=218 (array=218), bNumInterfaces=4
IAD: firstIf=0 count=2 (CDC)  /  IAD: firstIf=2 count=2 (Audio, proto=0x20)
IF3 alt1: EP 0x03 iso async mps=1024 bInterval=1 + EP 0x84 iso feedback mps=4
AS if 3 correctly links to Input Terminal 2 (USB Streaming)
errors=0 warnings=0
```

CDC 自己带 IAD（`CDC_ACM_DESCRIPTOR_INIT` 头 8 字节就是 IAD），Audio 也带，
两个功能在 `bDeviceClass=0xEF/02/01` 的复合设备里都是合法的。所以**别再改描述符了**，
问题从来不在那里。

---

## 3. 改动后的数据流（同步 OUT，96k/32bit/立体声）

```
主机每 125us 发 1 包(96B, DATA0)
   → SIE 收进 UEP3_RX_DMA（用户缓冲，1024B）
   → UIF_TRANSFER + TOKEN=OUT + EP=3
   → 锁存 RX_LEN=96，清 TRANSFER|ISO_ACT
   → handle_non_ep0_out()：不做 NAK、不看 TOG_OK
   → Audio_out_callback(nbytes=96)
   → 换缓冲 + usbd_ep_start_read(EP3, 新缓冲, 1024)
        → UEP3_RX_DMA = 新缓冲，RX_CTRL = DATA0|NYET
```

反馈端点(0x84)同理：`Audio_feedback_callback` 里立刻 `usbd_ep_start_write()`，
同步 IN 允许随时刷新（不做"必须处于 NAK"的检查），写完仍是 DATA0 翻转。

---

## 4. 上板怎么验证

1. 编译烧写（`obj/LittleTail.hex` 已经是这次修复后重新生成的干净构建）。
   > 提醒：`obj/` 里原来混着 10/1 02:27 那版**诊断固件**的 .o（`ch32v30x_it.o` 引用了
   > 已经删掉的 `USBHS_DiagSample`），和当前源码不一致；命令行下 `make clean` 因为
   > 找不到 `rm` 是**静默无效**的。我手工删了 `obj` 下的 `.o/.d` 再全量重编，
   > 现在 obj 和源码是一致的。
2. 插上后看设备管理器：应同时出现 `USB 串行设备(COMx)` 和 `扬声器/耳机`。
3. 播放 96k/32bit 音乐，Watch 窗口加这几个：
   | 变量 | 期望 |
   |---|---|
   | `g_ch32_usbhs_udc[0].ep_out[3].actual_xfer_len` | 每次回调 ≈96（96k/32bit 立体声） |
   | `g_audio_frames_total`（或你自己的计数值） | 持续增长 |
   | `USBHSD->INT_FG` | 读取时最高位不该常驻 0x40（ISO_ACT 我们每进中断都清） |
   | `USBHSD->ENDP_TYPE` | bit3=1(UEP3 同步 OUT)、bit20=1(UEP4 同步 IN) |
   | `USBHSD->ENDP_CONFIG` | bit19=1、bit20=1、bit17/18=1（CDC 两条）、bit1=1 |
   **不要下断点**：断点会让端点停在 NAK，主机会直接丢掉整个流，看到的次数是假的。

---

## 5. 如果还不对，按这个顺序试（各一行）

| 现象 | 改哪里 |
|---|---|
| 音频 OUT 回调仍然只进一次（抓包正常、串口正常） | `usb_dc_usbhs.c` 顶部 `#define CH32_USBHS_ISO_ACT_INT 1` 再编一次 —— 这颗片子的同步完成事件走 ISO_ACT 而不是 TRANSFER 时用这个。打开后中断里会同时清 ISO_ACT，不会再风暴 |
| 音频 OUT 一包都收不到、回调一次都不进 | `#define CH32_USBHS_ISO_RES USBHS_UEP_R_RES_ACK`（改掉默认的 NYET）。同步端点的"允许收发"响应码 ACK/NYET 两种写法在不同批次的片子上都有人用成功 |
| 播放正常但 Windows 仍报代码 10 | 把 `usbd_desc.h` 里 PID 从 `0x0000` 改成 `0x0001` 强制新驱动实例（Windows 按 VID/PID 缓存驱动），再"卸载设备+删除驱动程序" |

改完任意一条都要**重新全量编译**（我上面说的 stale object 问题）。

---

## 6. 关于旧文档 `USB_UAC2_修复说明.md` 第 9 节的更正

第 9 节（"回调只进一次的真正原因：UIF_ISO_ACT"）的**现象描述是对的，结论不完整**：

* 把 ISO 完成完全归给 `UIF_ISO_ACT` 是不对的 —— 沁恒那套量产 384k UAC2 固件的
  `INT_EN` 里**没有** ISO_ACT，它的同步 OUT 数据就是在 `UIF_TRANSFER` 分支里收的；
  TinyUSB 的 CH32 USBHS 端口同样不使能 ISO_ACT。两条独立实现都说明
  **同步完成会置 `UIF_TRANSFER`**。
* 真正的第一顺位原因是 2.1（DATA 翻转 + `TOG_OK` 过滤）和 2.2（同步端点被 NAK），
  它们足以单独造成"回调只进一次"。
* 第 9 节给的做法"使能 `UIE_ISO_ACT`"如果没有配套"每次进中断写 1 清掉"，就会变成
  第 2.3 节表格里的那种风暴。现在的代码把两件事**分成两个独立开关**，默认只做"清"，
  不做"使能"。
