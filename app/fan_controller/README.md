# 风扇控制器 (Fan Controller)

基于 ESP32-S3 的双路风扇控制器，支持 PWM 调速、转速检测、电压监测、WiFi 配置和 Web 管理界面。

## 硬件平台

- **开发板**: ESP32-S3-WROOM-N16R8
- **Flash**: 16MB
- **PSRAM**: 8MB

## GPIO 引脚分配

### 风扇 1 (Fan 1)

| 功能 | GPIO | 说明 |
|------|------|------|
| PWM 输出 | GPIO8 | 25kHz PWM 信号 (LEDC 通道 0) |
| 电源控制 | GPIO10 | 风扇电源开关，高电平有效 |
| 转速检测 | GPIO6 | 风扇转速脉冲输入，带内部上拉 |
| 电压检测 | GPIO4 | ADC1_CH3，通过分压电阻检测风扇电压 |

### 风扇 2 (Fan 2)

| 功能 | GPIO | 说明 |
|------|------|------|
| PWM 输出 | GPIO9 | 25kHz PWM 信号 (LEDC 通道 1) |
| 电源控制 | GPIO11 | 风扇电源开关，高电平有效 |
| 转速检测 | GPIO7 | 风扇转速脉冲输入，带内部上拉 |
| 电压检测 | GPIO5 | ADC1_CH4，通过分压电阻检测风扇电压 |

### 系统功能

| 功能 | GPIO | 说明 |
|------|------|------|
| 状态 LED | GPIO2 | 系统状态指示灯 |
| BOOT 按钮 | GPIO0 | 启动/配置按钮，带内部上拉 |
| UART0 TX | GPIO43 | 串口调试输出 |
| UART0 RX | GPIO44 | 串口调试输入 |

## ADC 电压检测说明

电压检测使用电阻分压电路：
- 分压比：35kΩ / 10kΩ（输出电阻 10kΩ，总电阻 45kΩ）
- 量程：0-12V 输入对应 0-2.67V ADC 输入
- 分辨率：12位
- 增益：1/4
- 参考电压：内部参考

## 软件功能

### 核心功能

- **双路独立控制**: 两路风扇可独立设置转速和控制模式
- **PWM 调速**: 25kHz 标准风扇 PWM 频率，0-100% 调速范围
- **转速检测**: 通过转速信号线实时监测风扇实际转速 (RPM)
- **电压监测**: ADC 检测风扇供电电压，支持曲线校准
- **电源控制**: 每路风扇独立电源开关

### 控制模式

1. **手动模式**: 直接设置目标 PWM 百分比
2. **ADC 目标模式**: 通过 ADC 反馈自动调节 PWM 以达到目标电压
3. **目标转速模式**: 根据转速反馈自动调节 PWM 以达到目标 RPM

### 网络功能

- **WiFi AP 模式**: 默认热点 `fanctl-XXXX`，密码 `fancontrol123`，IP `192.168.4.1`
- **WiFi STA 模式**: 可配置连接到现有 WiFi 网络
- **Web 管理界面**: HTTP 服务器提供风扇控制和状态监控
- **SSH 服务器**: 远程命令行访问（可选）

### 数据持久化

- 风扇配置自动保存到 Flash
- 支持校准曲线配置文件
- WiFi 凭证持久化存储

### 主机控制

- **存活检测**: 可选的主机心跳检测，超时自动处理
- **超时处理**: 可配置超时时间和超时行为

## 访问方式

### Web 界面

连接热点后访问：http://192.168.4.1/

### 串口 Shell

波特率 115200，使用命令：
```
fanctl status     # 查看风扇状态
fanctl set 0 50   # 设置风扇1转速为50%
fanctl set 1 75   # 设置风扇2转速为75%
```

### SSH 访问（如果启用）

```bash
ssh root@192.168.4.1 -p 2222
```

## 配置文件

- **应用配置**: `/cfg/config.json`
- **校准曲线**: `/cfg/adc_to_voltage.json`, `/cfg/voltage_to_percent.json`, `/cfg/percent_to_pwm.json`, `/cfg/percent_to_rpm.json`
- **SSH 配置**: `/cfg/ssh_config.json`
- **授权密钥**: `/cfg/authorized_keys`

## 编译和烧录

```bash
# 编译
west build -b esp32s3_fan_controller/appcpu app/fan_controller -d build_fan_controller

# 烧录
west flash -d build_fan_controller

# 或使用 esptool
esptool --chip esp32s3 --port COMx --baud 921600 write_flash 0x0 build_fan_controller/zephyr/zephyr.bin
```

## 硬件连接图

```
ESP32-S3
┌─────────────────┐
│                 │
│  GPIO8  ────────┼──► Fan1 PWM
│  GPIO10 ────────┼──► Fan1 Power
│  GPIO6  ────────┼──◄ Fan1 Tach
│  GPIO4  ────────┼──◄ Fan1 Voltage (ADC)
│                 │
│  GPIO9  ────────┼──► Fan2 PWM
│  GPIO11 ────────┼──► Fan2 Power
│  GPIO7  ────────┼──◄ Fan2 Tach
│  GPIO5  ────────┼──◄ Fan2 Voltage (ADC)
│                 │
│  GPIO2  ────────┼──► Status LED
│  GPIO0  ────────┼──◄ BOOT Button
│                 │
│  GPIO43 ────────┼──► UART TX
│  GPIO44 ────────┼──◄ UART RX
│                 │
└─────────────────┘
```

## 注意事项

1. 风扇 PWM 信号为 3.3V 电平，大部分 4 线风扇可直接兼容
2. 电压检测分压电路需根据实际风扇电压选择合适阻值
3. 转速检测线建议加上拉电阻（软件已配置内部上拉）
4. 电源控制 GPIO 驱动能力有限，建议通过 MOS 管或继电器控制风扇电源
