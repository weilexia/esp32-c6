# Waveshare ESP32-C6-Touch-LCD-1.83 硬件与开发参考手册

> **用途**：供开发者与 AI 编程助手快速查阅的板级硬件规范、完整 GPIO 引脚映射、总线共享约束、I2C 地址表及 ESP-IDF 开发要点。
> **信息来源**：微雪官方文档（`https://docs.waveshare.net/ESP32-C6-Touch-LCD-1.83/`）、官方引脚图及硬件原理图（`Rev1.2`）。
> **SKU**：`32793`（ESP32-C6-Touch-LCD-1.83） / `32794`（ESP32-C6-Touch-LCD-1.83-EN）

---

## 1. 核心硬件规格总览

| 项目 | 规格参数 |
| :--- | :--- |
| **主控芯片 (SoC)** | **ESP32-C6**（32 位单核 RISC-V，最高主频 160 MHz） |
| **片上内存** | 512 KB HP SRAM，320 KB ROM，16 KB LP SRAM |
| **外部 Flash** | **16 MB NOR Flash**（`W25Q128JVSIQ`） |
| **无线连接** | 2.4 GHz Wi-Fi 6 (802.11 b/g/n/ax)、Bluetooth 5 (LE)、IEEE 802.15.4（Zigbee 3.0 / Thread），板载贴片天线 |
| **显示屏** | 1.83 英寸 IPS 电容触摸圆角 LCD，分辨率 **240(H) × 284(V)**，65K 色（常用 RGB565） |
| **显示驱动 IC** | **ST7789P**（4 线 SPI，单向写无 MISO；内部 RAM 为 240×320，仅使用 240×284） |
| **触摸驱动 IC** | **CST816D**（自电容触控，标准 I2C，10 kHz ~ 400 kHz） |
| **音频编解码 (播放)** | **ES8311** 低功耗音频编解码芯片 + **NS4150B** 功放芯片 + MX1.25 喇叭接口（不区分极性） |
| **音频采集 (录音/AEC)** | **ES7210** 多路 ADC 芯片 + 贴片双麦克风阵列（支持硬件回声消除回路） |
| **六轴 IMU** | **QMI8658C**（3 轴加速度计 + 3 轴陀螺仪，支持姿态检测、计步） |
| **实时时钟 (RTC)** | **PCF85063ATL**（外接 32.768 kHz 晶振，由 AXP2101 电池回路不间断供电） |
| **电源管理 (PMIC)** | **AXP2101**（多路可配置电压输出、锂电池充放电管理） |
| **电池接口** | 1.2mm 2PIN 接口，支持 3.7V 锂电池充放电（搭配外壳推荐 `6 × 25 × 25 mm` 电池） |
| **存储扩展** | 板载 **Micro SD (TF)** 卡槽（SPI 模式） |
| **按键** | **BOOT** 侧键（`GPIO9`）、**PWR** 电源侧键（`GPIO18`，短按开机 / 长按 6 秒关机，支持自定义功能） |
| **USB 接口** | Type-C（直连 ESP32-C6 原生 USB Serial/JTAG，用于烧录和日志打印） |

---

## 2. 完整 GPIO 引脚映射表（按 GPIO 编号）

| GPIO | 信号名称 | 连接外设 / 功能 | 备注 / 冲突提示 |
| :--- | :--- | :--- | :--- |
| `GPIO0` | `PA_CTRL` | 音频功放 `NS4150B` 使能控制 | 播放音频前需拉高使能功放 |
| `GPIO1` | `LCD_SCL` / `SD_SCK` | **共享 SPI 时钟 (SCK)** | LCD (`ST7789P`) 与 Micro SD 共用 |
| `GPIO2` | `LCD_SDA` / `SD_MOSI` | **共享 SPI 数据输出 (MOSI)** | LCD (`ST7789P`) 与 Micro SD 共用 |
| `GPIO3` | `LCD_DC` | LCD 数据/命令选择 (`D/CX`) | `0` = 命令，`1` = 数据 |
| `GPIO4` | `LCD_RST` / `TP_RST` | **共享复位引脚** | LCD (`ST7789P`) 与触摸 (`CST816D`) 共用复位 |
| `GPIO5` | `LCD_CS` | LCD SPI 片选 (`CSX`) | 低电平有效 |
| `GPIO6` | `LCD_BL` | LCD 背光控制 | 驱动 MOSFET `Q3 (DMG1012T)`，支持 PWM 调光 |
| `GPIO7` | `ESP32_SDA` | **共享 I2C SDA** | 板载 2.2kΩ 上拉至 `VCC3V3`，挂载 6 个 I2C 器件 |
| `GPIO8` | `ESP32_SCL` | **共享 I2C SCL** | 板载 2.2kΩ 上拉至 `VCC3V3`，挂载 6 个 I2C 器件 |
| `GPIO9` | `BOOT` | BOOT 侧边按键 | 低电平有效；按住上电进入下载模式 |
| `GPIO10` | `QMI_INT1` | 六轴 IMU (`QMI8658C`) 中断 1 | — |
| `GPIO11` | `TP_INT` | 触摸芯片 (`CST816D`) 中断 | — |
| `GPIO12` | `USB_N` | 原生 USB D-（Type-C） | 同时引出到预留焊盘 |
| `GPIO13` | `USB_P` | 原生 USB D+（Type-C） | 同时引出到预留焊盘 |
| `GPIO15` | `RTC_INT` | RTC (`PCF85063`) 中断 | — |
| `GPIO16` | `SD_MISO` (`U0TXD`) | Micro SD SPI MISO | 与 UART0 TX 及预留焊盘共用 |
| `GPIO17` | `SD_CS` (`U0RXD`) | Micro SD SPI 片选 (`SDCS`) | 与 UART0 RX 及预留焊盘共用 |
| `GPIO18` | `PWR` (`SYS_OUT`) | 电源按键状态检测 | 可作自定义按键输入 |
| `GPIO19` | `I2S_MCLK` | 音频 I2S 主时钟 (`MCLK`) | `ES8311` 与 `ES7210` 共用 |
| `GPIO20` | `I2S_SCLK` | 音频 I2S 位时钟 (`BCLK`/`SCLK`) | `ES8311` 与 `ES7210` 共用 |
| `GPIO21` | `I2S_ASDOUT` | 音频 I2S 数据输入 (`DIN` -> ESP32) | 接收 `ES7210` / `ES8311` 录音数据 |
| `GPIO22` | `I2S_LRCK` | 音频 I2S 帧时钟 (`WS`/`LRCK`) | `ES8311` 与 `ES7210` 共用 |
| `GPIO23` | `I2S_DSDIN` | 音频 I2S 数据输出 (ESP32 -> `DOUT`) | 输出播放音频到 `ES8311` |

---

## 3. 按功能模块分组的引脚与配置

### 3.1 显示屏（ST7789P，4 线 SPI）
- **分辨率**：`240 × 284`（圆角屏，四角边缘像素不可见；ST7789P 显存为 240×320，驱动需注意设置合适的 Y 轴偏移）
- **颜色格式**：RGB565（16-bit，推荐） / RGB444 / RGB666
- **SPI 模式**：Mode 0（`CPOL = 0, CPHA = 0`，MSB First）
- **引脚**：`SCK=GPIO1`, `MOSI=GPIO2`, `DC=GPIO3`, `RST=GPIO4`, `CS=GPIO5`, `BL=GPIO6`（无 MISO）

### 3.2 共享 I2C 总线与设备地址表
- **总线引脚**：`SDA = GPIO7`，`SCL = GPIO8`（板载 2.2kΩ 上拉电阻至 `VCC3V3`）

| 器件 | 功能 | 7-bit I2C 地址 | 中断/复位引脚 |
| :--- | :--- | :--- | :--- |
| **CST816D** | 电容触摸控制器 | `0x15` | `INT = GPIO11`, `RST = GPIO4`（与 LCD 共享） |
| **ES8311** | 音频编解码器 (播放) | `0x18`（CE 下拉 10kΩ） | — |
| **AXP2101** | 电源管理芯片 (PMIC) | `0x34` | — |
| **ES7210** | 音频 ADC / 回声消除 | `0x40`（原理图标注 `AD0=0, AD1=0`） | — |
| **PCF85063ATL** | 实时时钟 (RTC) | `0x51` | `INT = GPIO15` |
| **QMI8658C** | 6 轴 IMU (加速度+陀螺仪) | `0x6B`（原理图标注 `0x6B`） | `INT1 = GPIO10` |

### 3.3 音频子系统（ES8311 + ES7210 + NS4150B）
- **控制接口**：共享 I2C（`GPIO7`/`GPIO8`）
- **数据接口 (I2S)**：`MCLK=GPIO19`, `SCLK(BCLK)=GPIO20`, `LRCK(WS)=GPIO22`, `DOUT(DSDIN)=GPIO23`, `DIN(ASDOUT)=GPIO21`
- **功放使能**：`PA_CTRL = GPIO0`（控制 `NS4150B` 功放，驱动 MX1.25 喇叭）

### 3.4 Micro SD 卡槽（SPI 模式）
- **引脚**：`SCK=GPIO1`, `MOSI=GPIO2`, `MISO=GPIO16`, `CS=GPIO17`
- **注意**：与 LCD 共用 SPI 总线（`SCK`/`MOSI`），必须挂载在同一 `SPI2_HOST` 下。

### 3.5 预留扩展焊盘定义（双列 2×4，自上而下）

| 左列 | 右列 |
| :--- | :--- |
| `ESP32_SCL` (`GPIO8`) | `GND` |
| `ESP32_SDA` (`GPIO7`) | `LCD_3V3` |
| `USB_N` (`GPIO12`) | `GPIO17` (`U0RXD` / `SDCS`) |
| `USB_P` (`GPIO13`) | `GPIO16` (`U0TXD` / `SD_MISO`) |

---

## 4. C/C++ 引脚宏定义参考（可直接复用）

```c
#pragma once

// === Shared SPI Bus (LCD + Micro SD) ===
#define BOARD_SPI_HOST          SPI2_HOST
#define BOARD_SPI_SCK_GPIO      1
#define BOARD_SPI_MOSI_GPIO     2
#define BOARD_SPI_MISO_GPIO     16

// === LCD (ST7789P, 240x284) ===
#define BOARD_LCD_WIDTH         240
#define BOARD_LCD_HEIGHT        284
#define BOARD_LCD_DC_GPIO       3
#define BOARD_LCD_RST_GPIO      4   // Shared with Touch RST
#define BOARD_LCD_CS_GPIO       5
#define BOARD_LCD_BL_GPIO       6

// === Micro SD Card (SPI) ===
#define BOARD_SD_CS_GPIO        17

// === Shared I2C Bus (Touch, PMIC, IMU, RTC, ES8311, ES7210) ===
#define BOARD_I2C_SDA_GPIO      7
#define BOARD_I2C_SCL_GPIO      8

#define BOARD_I2C_ADDR_CST816D  0x15
#define BOARD_I2C_ADDR_ES8311   0x18
#define BOARD_I2C_ADDR_AXP2101  0x34
#define BOARD_I2C_ADDR_ES7210   0x40
#define BOARD_I2C_ADDR_PCF85063 0x51
#define BOARD_I2C_ADDR_QMI8658  0x6B

// === Touch (CST816D) ===
#define BOARD_TOUCH_RST_GPIO    4   // Shared with LCD RST
#define BOARD_TOUCH_INT_GPIO    11

// === Audio I2S (ES8311 + ES7210) & PA (NS4150B) ===
#define BOARD_I2S_MCLK_GPIO     19
#define BOARD_I2S_BCLK_GPIO     20
#define BOARD_I2S_DIN_GPIO      21  // I2S_ASDOUT (ESP32 RX)
#define BOARD_I2S_WS_GPIO       22  // I2S_LRCK
#define BOARD_I2S_DOUT_GPIO     23  // I2S_DSDIN  (ESP32 TX)
#define BOARD_PA_CTRL_GPIO      0

// === Sensors & RTC Interrupts ===
#define BOARD_IMU_INT1_GPIO     10
#define BOARD_RTC_INT_GPIO      15

// === Buttons ===
#define BOARD_BTN_BOOT_GPIO     9
#define BOARD_BTN_PWR_GPIO      18
```

---

## 5. 开发关键注意事项（AI 编程避坑指南）

1. **ESP-IDF 目标芯片与配置**：
   - 芯片目标必须设置为 `esp32c6`（`idf.py set-target esp32c6`）。
   - Flash 大小为 **16MB**，需在 `menuconfig`（`Serial flasher config -> Flash size`）中配置为 `16 MB`。
   - 控制台日志输出应使用 **USB Serial/JTAG Controller**（`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`），不要占用 UART0（因为 `GPIO16`/`GPIO17` 接在 Micro SD 卡槽上）。
2. **LCD 与 SD 卡共用 SPI 总线**：
   - `ST7789P` 和 `Micro SD` 共享 `GPIO1 (SCK)` 与 `GPIO2 (MOSI)`。
   - 同时使用时，只需调用一次 `spi_bus_initialize(SPI2_HOST, ...)`（同时配置 `miso_io_num = 16`），然后分别挂接 LCD 设备（`CS=5`）和 SD 卡设备（`CS=17`）。
3. **LCD 与触摸屏共用复位引脚 (`GPIO4`)**：
   - `LCD_RST` 和 `TP_RST` 物理短接在 `GPIO4`。
   - 初始化时避免后初始化的驱动再次拉低 `GPIO4` 导致先初始化的器件复位失效（建议统一硬件复位一次后，在其中一个驱动配置中将 `rst_gpio_num` 设为 `-1`）。
4. **I2C 共享总线线程安全**：
   - 6 个外设共用一条 I2C 总线（`GPIO7`/`GPIO8`），在 FreeRTOS 多任务访问时需使用 ESP-IDF 新版 `driver/i2c_master.h`（自带总线互斥）或自行加互斥锁。
5. **烧录与进入下载模式**：
   - 正常通过 Type-C 口烧录；若遇到“等待上电同步中”或无法识别，**按住侧边 `BOOT` 键（`GPIO9`）再重新上电/复位** 即可进入下载模式。
   - 官方合并测试固件烧录地址为 `0x00`，波特率可用 `1152000`。
6. **电源管理 (AXP2101)**：
   - 外设供电 rail 由 `AXP2101` 管理（`DCDC1 = VCC3V3`, `ALDO1/2 = 3.3V`, `ALDO3 = 3.0V`, `ALDO4 = 1.8V`, `BLDO2 = 2.8V`, `RTCLDO = VCC-RTC`）。如果外设无响应或电池电量需要读取，需先初始化 AXP2101。

---

## 6. 物理尺寸（单位：mm）

- **整机外壳尺寸**：`37.20 (宽) × 44.20 (高) × 19.00 (厚，含屏幕)`（侧框厚 `17.70`）
- **屏幕可视区域**：`29.90 (宽) × 35.30 (高)`
- **背面螺丝孔距**：`26.40 (水平) × 33.00 (垂直)`

---

## 7. 官方示例与资料链接

- **推荐 ESP-IDF 版本**：`v5.5.x`（微雪文档示例基于 `v5.5.2`）
- **官方 ESP-IDF 示例列表**（位于 GitHub `examples/` 目录）：
  1. `lvgl_brookesia_01`：LVGL + ESP-Brookesia 类手机 APP 界面
  2. `i2s_es7210_rec_02`：ES7210 麦克风录音并保存 WAV 到 TF 卡
  3. `mp3_play_03`：ES8311 播放 TF 卡根目录 `1.mp3`
  4. `lvgl_example_04`：LVGL + 触摸屏官方 Demo（`lv_demo_stress`）
  5. `AXP2101_05`：AXP2101 电源管理与电池信息打印
  6. `lvgl_image_06`：LVGL 手势左右滑动切换图片
- **官方文档与下载**：
  - 文档主页：https://docs.waveshare.net/ESP32-C6-Touch-LCD-1.83/
  - ESP-IDF 开发文档：https://docs.waveshare.net/ESP32-C6-Touch-LCD-1.83/ESP-IDF/
  - GitHub 示例仓库：https://github.com/waveshareteam/ESP32-C6-Touch-LCD-1.83/tree/main
  - 示例程序压缩包：https://files.waveshare.net/wiki/ESP32-C6-Touch-LCD-1.83/ESP32-C6-Touch-LCD-1.83-Demo.zip
  - 硬件原理图 (Rev1.2 PDF)：https://www.waveshare.net/w/upload/2/2f/ESP32-C6-Touch-LCD-1.83_Rev1.2.pdf
  - 芯片数据手册：
    - ESP32-C6 规格书 (中文)：https://documentation.espressif.com/esp32-c6_datasheet_cn.pdf
    - ESP32-C6 技术参考手册 (中文)：https://documentation.espressif.com/esp32-c6_technical_reference_manual_cn.pdf
    - QMI8658C：https://www.waveshare.net/w/upload/5/5f/QMI8658C.pdf
    - PCF85063A：https://www.waveshare.net/w/upload/9/97/PCF85063A.pdf
    - AXP2101：https://www.waveshare.net/w/upload/e/ed/X-power-AXP2101_SWcharge_V1.0.pdf
    - ES8311 数据手册：https://www.waveshare.net/w/upload/6/65/ES8311.DS.pdf
    - ES7210 数据手册：https://www.waveshare.net/w/upload/5/54/ES7210-datasheet.pdf
