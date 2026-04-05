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

波特率 115200，基本命令：
```shell
fanctl status             # 查看风扇状态
fanctl set 1 50 on        # 运行时设置风扇1为 50%
fanctl set 2 75 off       # 运行时设置风扇2为 75%，并关闭输出
show all                  # 查看系统、网络、存储、SSH、NTP、风扇等状态
show system
show wifi
show storage
cf                        # 查看文件系统容量/剩余空间
duf                       # cf 的别名

# 文件系统命令
cd /etc/fanctl              # 切换目录
pwd                         # 显示当前目录
ls                          # 列出文件
ls /etc/fanctl/curves       # 列出指定目录
cat ./config.json           # 查看文件内容
mkdir test                  # 创建目录
touch note.txt              # 创建空文件
rm file.txt                 # 删除文件
writefile config.json {"key":"value"}  # 写入文件

# 编辑器命令
edit open config.json  # 打开文件
edit show              # 显示内容
edit set 1 "new line"  # 修改行
edit write             # 保存
edit quit              # 退出
```

### SSH 访问（如果启用）

```bash
ssh root@192.168.4.1 -p 22
```

默认用户名和密码见 [`config/ssh_config.json`](./config/ssh_config.json)。

## WiFi 配置

### 方式 1：使用 WiFi 连接工具（推荐）

通过串口 Shell 使用交互式 WiFi 连接工具：

```shell
# 扫描周围 WiFi 网络
fanctl scan

# 示例输出：
# Found 4 network(s):
# No.  SSID                             RSSI     CH     Security
# 1    MyHomeWiFi                       -45      6      WPA/WPA2
# 2    GuestNetwork                     -72      11     Open
# 3    Office_WiFi                      -68      1      WPA3
# 4    xiaomi_5G                        -55      36     WPA/WPA2

# 使用工具连接（先扫描显示列表）
wificonnect

# 或者直接连接指定网络（通过序号）
wificonnect 1 mypassword

# 连接开放网络（无需密码）
wificonnect 2
```

### 方式 2：使用传统命令

```shell
# 连接加密 WiFi
fanctl wifi MyHomeWiFi mypassword

# 连接开放 WiFi
fanctl wifi OpenNetwork

# 查看连接状态
fanctl status

# 开关 AP 热点
fanctl ap on   # 开启热点
fanctl ap off  # 关闭热点

# 清除保存的 WiFi 密码
fanctl clearwifi
```

### 方式 3：通过 Web 界面

连接设备热点后访问 http://192.168.4.1/，在 Web 界面中配置 WiFi。

### HTTP API 方式

```bash
# 扫描 WiFi 网络
curl -X POST http://192.168.4.1/api/wifi/scan

# 连接到指定 WiFi
curl -X POST http://192.168.4.1/api/wifi -d "ssid=MyWiFi&psk=password"
```

### AP 模式配置

AP/STA 参数保存在 `/etc/fanctl/wifi.json`：

```json
{
  "sta_ssid": "",
  "sta_psk": "",
  "ap_enabled": true
}
```

- `ap_enabled: true` (默认): 启动时开启 AP 热点 + 尝试连接 STA
- `ap_enabled: false`: 仅 STA 模式，不开启 AP 热点

## 配置文件

- **应用配置**: `/etc/fanctl/config.json`
- **字段定义**: `/etc/fanctl/config.fields.json`
- **WiFi 配置**: `/etc/fanctl/wifi.json`
- **NTP 配置**: `/etc/fanctl/ntp.json`
- **校准曲线**: `/etc/fanctl/curves/adc_to_voltage.json`, `/etc/fanctl/curves/voltage_to_percent.json`, `/etc/fanctl/curves/percent_to_pwm.json`, `/etc/fanctl/curves/percent_to_rpm.json`
- **SSH 配置**: `/etc/ssh/sshd_config.json`
- **SSH 主机密钥**: `/etc/ssh/ssh_host_ecdsa_key.der`
- **授权密钥**: `/root/.ssh/authorized_keys`

## 恢复出厂设置

通过串口 Shell 或 SSH 执行：

```shell
fanctl factoryreset confirm
```

执行后会：

- 恢复默认应用配置、WiFi 配置、NTP 配置、SSH 配置
- 恢复默认风扇曲线文件
- 清空 `authorized_keys`
- 删除运行时生成的 SSH host key
- 清理设置存储并自动重启设备

该命令必须带 `confirm`，否则只会提示警告，不会真的执行。

## 编译和烧录

### 使用 Pixi（推荐）

```bash
# 增量编译
pixi run fan_controller_build

# 重新配置并完整编译
pixi run fan_controller_config

# 清理 build 目录
pixi run fan_controller_clean

# 擦除整片 Flash
pixi run fan_controller_erase

# 烧录当前镜像
pixi run fan_controller_flash

# 增量编译并烧录
pixi run fan_controller_deploy

# 完整重新配置并烧录
pixi run fan_controller_deploy_clean
```

兼容别名：

```bash
pixi run fan_controller      # 等价于 fan_controller_build
pixi run fan_controller_c    # 等价于 fan_controller_config
```

默认串口为 `COM9`。如需修改：

```powershell
$env:FANCTL_PORT = "COM7"
pixi run fan_controller_deploy
```

### 使用 West

```bash
# 编译
west build -b esp32s3_fan_controller/esp32s3/procpu app/fan_controller -d build_fan_controller -- -DBOARD_ROOT=%PIXI_PROJECT_ROOT%

# 或使用 esptool
esptool --chip esp32s3 --port COMx --baud 921600 erase-flash
esptool --chip esp32s3 --port COMx --baud 921600 --before default-reset --after hard-reset write-flash -u --flash-mode dio --flash-freq 80m --flash-size 16MB 0x0 build_fan_controller/zephyr/zephyr.bin
```

当前工程更推荐使用 `pixi` 任务或直接 `esptool` 烧录；不要依赖旧的 `west flash` 工作流。

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
