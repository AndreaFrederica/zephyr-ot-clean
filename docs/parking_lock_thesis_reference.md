# 基于RS485与物联网的车位锁远程监控系统设计与实现

> **论文参考文档**  
> 本文档为学位论文中“系统设计与实现”章节的详细参考资料，涵盖总体架构、各模块硬件/软件设计、通信协议、关键算法与实现细节。所有设计均对应 `app/parking_lock_slave`、`app/parking_lock_gateway`、`app/parking_lock_espwifi`、`app/parking_lock_cloud` 四个子系统的实际代码实现。

---

## 摘要

本系统针对地下车库车位锁的智能化管理需求，设计并实现了一套“主从分层、现场总线+物联网”的远程监控方案。现场层采用STM32F103微控制器构建RS485半双工总线网络，从机节点负责环境感知（温湿度、火焰检测）与车位锁执行控制，网关主机负责总线轮询、设备注册与协议汇聚；网络层采用ESP8266 WiFi模块实现MQTT透传；应用层基于FastAPI构建云端服务，提供RESTful API与WebSocket实时数据推送，前端采用原生JavaScript实现响应式监控界面。系统实现了节点自动注册、状态实时上报、远程锁定/解锁、离线检测与自动重试等功能。

---

# 第一章 系统总体架构设计

## 1.1 设计目标与功能需求

### 1.1.1 功能需求分析

1. **环境感知**：从机节点周期采集地下车库的温度、湿度、火焰数字量与模拟量。
2. **执行控制**：根据网关或云端指令，驱动舵机完成车位锁的升起（锁定）与降下（解锁）。
3. **现场通信**：主从节点通过RS485半双工总线进行可靠通信，支持单主机多从机拓扑。
4. **人机交互**：网关连接串口屏（Nextion HMI），实时显示各节点状态并支持本地触摸控制。
5. **远程联网**：通过ESP8266模块连接WiFi与MQTT Broker，将现场数据上传至云端。
6. **远程监控**：云端提供Web界面，支持实时数据查看、历史事件追溯、远程控制与告警提示。

### 1.1.2 非功能需求

- **可扩展性**：RS485总线支持最多64个从机节点，网关内存中预留64个节点状态槽位。
- **实时性**：周期轮询间隔200ms，单个节点状态更新延迟可控制在秒级。
- **可靠性**：具备总线冲突退避、节点离线检测（15s超时）、DHT11读取干扰缓存、前端命令自动重试等机制。
- **可维护性**：采用模块化设计，各层通过文本协议解耦，便于独立调试与升级。

## 1.2 硬件总体架构

系统硬件分为三个层级：

### 1.2.1 现场层硬件

- **从机板（`stm32f103_ot_sensor`）**：
  - MCU：STM32F103C8T6
  - 传感器：DHT11（单总线数字温湿度）、火焰数字输出模块（GPIO输入）、火焰模拟输出模块（ADC输入）
  - 执行器：SG90舵机（PWM信号，PA1）
  - 通信：RS485收发器连接USART3（PB10/PB11）
  - 供电：5V/3.3V双电源

- **网关板（`stm32f103_ot_gateway`）**：
  - MCU：STM32F103C8T6
  - 通信接口：
    - USART3（PB10/PB11）→ RS485总线
    - USART1（PA9/PA10）→ Nextion串口屏
    - USART2（PA2/PA3）→ ESP8266 WiFi模块

### 1.2.2 网络层硬件

- **ESP8266模块**（NodeMCU或类似开发板）：
  - 运行自定义Arduino固件
  - 通过UART与网关主机连接，波特率115200
  - 负责WiFi STA连接、MQTT客户端、网络配置持久化

### 1.2.3 应用层硬件

- **云端服务器**：运行Python FastAPI的通用x86服务器或PC
- **MQTT Broker**：可选外部Mosquitto或嵌入式amqtt
- **用户终端**：带浏览器的PC或移动设备

## 1.3 软件总体架构

```
┌─────────────────────────────────────────────────────────────────┐
│                        应用层 (Application)                      │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────┐  │
│  │  Web前端     │  │ FastAPI服务  │  │  MQTT Broker         │  │
│  │ (JS/HTML)    │  │ (Python)     │  │ (Mosquitto/amqtt)    │  │
│  └──────────────┘  └──────────────┘  └──────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
                              ↑ MQTT over WiFi
┌─────────────────────────────────────────────────────────────────┐
│                        网络层 (Network)                          │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │              ESP8266 (Arduino C++)                        │  │
│  │  UART ↔ WiFi/MQTT Bridge | LittleFS Config Persistence   │  │
│  └──────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
                              ↑ USART2 115200 ASCII
┌─────────────────────────────────────────────────────────────────┐
│                        网关层 (Gateway)                          │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │         STM32 Gateway (Zephyr RTOS / C++)                │  │
│  │  RS485调度 | Screen转发 | WiFi转发 | UID注册 | 离线检测  │  │
│  └──────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
                              ↑ RS485 半双工
┌─────────────────────────────────────────────────────────────────┐
│                        现场层 (Field)                            │
│  ┌────────┐  ┌────────┐  ┌────────┐        ┌────────┐          │
│  │ Slave 1│  │ Slave 2│  │ ...    │        │ Slave N│          │
│  │ Zephyr │  │ Zephyr │  │        │        │ Zephyr │          │
│  └────────┘  └────────┘  └────────┘        └────────┘          │
└─────────────────────────────────────────────────────────────────┘
```

## 1.4 通信链路拓扑

系统存在四条主要通信链路：

1. **RS485总线链路**：从机 ↔ 网关，半双工、一主多从、ASCII文本帧、`\r\n`帧尾。
2. **串口屏链路**：网关 ↔ Nextion，全双工、ASCII指令+0xFF 0xFF 0xFF结束码。
3. **网关-ESP链路**：网关 ↔ ESP8266，全双工、ASCII文本帧、`\r\n`帧尾。
4. **云端链路**：ESP8266 ↔ MQTT Broker ↔ FastAPI ↔ Web前端。

---

# 第二章 从机节点（Slave）设计与实现

## 2.1 软件架构与任务模型

从机固件基于Zephyr RTOS构建，但采用极简的单线程轮询模型（无额外内核线程）。整个系统由以下两部分组成：

- **中断服务程序（ISR）**：UART3接收中断，负责字节级接收、行组装与消息队列投递。
- **主循环（main thread）**：从`k_msgq`中取出完整命令行，进行解析、执行与响应。

这种设计充分利用了RS485低速通信的特点，避免了多线程同步的复杂性，同时满足实时性要求。

### 2.1.1 主循环伪代码

```cpp
int main() {
    init_rs485_uart();
    init_node_identity();          // 读取UID，node_id初始为0
    init_devices();                // 初始化DHT/GPIO/ADC/PWM
    register_uart_isr();

    char line[RX_BUF_SIZE];
    while (true) {
        while (k_msgq_get(&line_msgq, line, K_NO_WAIT) == 0) {
            handle_command(line);  // 命令解析与执行
        }
        k_msleep(1);               // 释放CPU
    }
}
```

## 2.2 节点身份与自动注册机制

### 2.2.1 硬件UID读取

从机上电时调用`hwinfo_get_device_id()`读取STM32内部96位唯一设备标识符（UID）。为减少总线流量，仅取前8字节（64位），格式化为16进制字符串（如`12AB34CD56EF7788`），作为节点的全局唯一身份标识。

```cpp
void init_node_identity() {
    uint8_t uid[16];
    ssize_t uid_len = hwinfo_get_device_id(uid, sizeof(uid));
    // 取前8字节，格式化为HEX字符串
}
```

### 2.2.2 地址分配状态机

从机的`node_id`初始值为0，表示“未注册”状态。在此状态下：
- 不响应针对具体地址的`REQ`命令（除非广播`REQ,0,DISCOVER`）。
- 收到`DISCOVER`后，执行随机退避（2~14ms），然后发送`REG,REQ,UID=...`请求注册。
- 收到`SET,0,ADDR,<UID>,<addr>`后，验证UID匹配，若匹配则设置`node_id = addr`，并回复`REG,ACK,addr,UID=...`。

## 2.3 UART接收中断与消息队列

### 2.3.1 ISR设计

从机使用Zephyr的UART中断驱动API。ISR中：
1. 调用`uart_irq_update()`确认中断源。
2. 调用`uart_irq_rx_ready()`循环读取FIFO中的字节。
3. 每读到一个字节，更新`rs485_last_activity_ms`（原子变量，用于总线空闲检测）。
4. 以`\r`或`\n`作为行结束符，将组装好的行复制到局部缓冲区，通过`k_msgq_put(&line_msgq, ..., K_NO_WAIT)`投递到主循环。

### 2.3.2 缓冲区管理

- `rx_line`：ISR中的行组装缓冲区，大小128字节。
- `line_msgq`：消息队列，深度为8，每条消息128字节。
- 若队列满或行超长，则计数`rx_overflows`，在后续上报中可作为诊断数据。

## 2.4 命令解析与处理

### 2.4.1 CSV拆分器

命令帧采用逗号分隔的文本格式。从机实现了一个轻量级的`split_csv`函数，将行原地拆分为最多8个字段指针。

```cpp
int split_csv(char *line, char *parts[PARTS_MAX]) {
    // 原地替换逗号为'\0'，返回字段数
}
```

### 2.4.2 命令路由表

| 命令 | 字段数 | 条件 | 动作 |
|------|--------|------|------|
| `REQ,node_id,PING` | ≥3 | node_id匹配或为0 | 延迟`(node_id-1)*8ms`后回复`ACK,node_id,PONG` |
| `REQ,node_id,GET` | ≥3 | node_id匹配 | 采集传感器并发送`REPORT` |
| `SET,node_id,LOCK,value` | ≥4 | node_id匹配 | 设置舵机，回复ACK，再发REPORT |
| `SET,0,ADDR,UID,addr` | ≥5 | node_id==0且UID匹配 | 设置node_id，回复`REG,ACK` |
| `REQ,0,DISCOVER` | ≥3 | node_id==0 | 随机退避后发送`REG,REQ` |

## 2.5 传感器采集模块

### 2.5.1 DHT11温湿度读取

DHT11通过Zephyr的`sensor`子系统访问。调用`sensor_sample_fetch()`触发读取，再分别获取`SENSOR_CHAN_AMBIENT_TEMP`和`SENSOR_CHAN_HUMIDITY`通道。读取结果转换为百分之一摄氏度与百分之一相对湿度，以定点数形式缓存。

**干扰缓存策略**：由于SG90舵机动作时电源纹波可能干扰DHT11，导致偶发读取失败。从机在`read_dht_values()`失败时，若存在上一次成功缓存（`has_dht_cache`），则使用缓存值替代异常值，避免上报`-1000.00`的尖峰数据。

### 2.5.2 火焰检测

- **数字量**：通过`gpio_pin_get_dt()`读取火焰传感器DO引脚。高电平为正常（无火），低电平为告警。
- **模拟量**：通过Zephyr ADC子系统读取AO引脚电压。使用`adc_sequence_init_dt()`配置序列，`adc_read_dt()`启动转换，`adc_raw_to_millivolts_dt()`转换为mV值。

### 2.5.3 数据上报格式

```text
REPORT,node_id,reason,T=temp,H=humi,FD=flame_digital,FA=flame_analog_mv,
L=lock_state,DE=dht_err,AE=adc_err,SE=servo_err,RB=rx_bytes,RL=rx_lines,
RC=rx_cmds,RO=rx_overflows,UID=uid_hex
```

其中`reason`为触发上报的原因，如`POLL`（轮询请求）、`SET`（锁定命令触发）、`PERIODIC`（周期上报，预留）等。

## 2.6 执行器控制模块

### 2.6.1 舵机驱动

车位锁舵机通过PWM控制角度。Zephyr的`pwm`API用于设置脉冲宽度：
- 锁定（升起）：2000μs脉冲（PWM_USEC(2000)）
- 解锁（降下）：1000μs脉冲（PWM_USEC(1000)）

```cpp
void set_lock(bool locked) {
    uint32_t pulse = locked ? PWM_USEC(2000) : PWM_USEC(1000);
    lock_servo_ret = pwm_set_pulse_dt(&lock_servo, pulse);
    if (lock_servo_ret == 0) lock_state = locked ? 1 : 0;
}
```

### 2.6.2 总线发送与冲突避免

RS485为半双工总线，从机仅在检测到总线空闲时才可发送。从机维护一个原子变量`rs485_last_activity_ms`，记录最近一次总线活动的系统时间戳。发送前检查：

```cpp
if (now - last_activity >= RS485_IDLE_GUARD_MS) { // 4ms
    uart_send_bytes(text);
    atomic_set(&rs485_last_activity_ms, now);
    return true;
}
```

若总线忙，则执行最多5次重试，每次退避时间根据`node_id`、尝试次数与当前时间低几位计算一个1~7ms的随机值，以降低多节点碰撞概率。

---

# 第三章 网关主机（Gateway）设计与实现

## 3.1 多路UART并发架构

网关主机需要同时处理三路UART通信：RS485（总线）、Screen（串口屏）、WiFi（ESP8266）。每一路都配备独立的接收ISR与消息队列：

```cpp
K_MSGQ_DEFINE(rs485_msgq, RX_BUF_SIZE, RX_MSGQ_DEPTH, 4);
K_MSGQ_DEFINE(screen_msgq, RX_BUF_SIZE, RX_MSGQ_DEPTH, 4);
K_MSGQ_DEFINE(wifi_msgq, RX_BUF_SIZE, RX_MSGQ_DEPTH, 4);
```

三路ISR共享同一个`uart_rx_isr()`函数，通过`user_data`参数传入各自的`UartRxContext`结构体，实现代码复用。

## 3.2 节点状态管理

### 3.2.1 节点状态结构体

网关维护一个大小为64的`states`数组，索引`i`对应节点地址`i+1`。

```cpp
struct NodeState {
    int id;           // 节点地址 1~64
    bool online;      // 在线状态
    int64_t lastSeen; // 上次收到帧的时间戳（ms）
    int32_t tempCenti;
    int32_t humiCenti;
    int flameDigital;
    int flameMv;
    int lockState;
};
```

同时维护一个`registered_uid[64][NODE_UID_MAX]`数组，记录每个地址对应的UID，用于验证节点身份与实现基于UID的自动注册。

### 3.2.2 UID-地址映射表操作

- `is_addr_registered(addr)`：判断某地址是否已绑定UID。
- `find_addr_by_uid(uid)`：根据UID查找已分配地址。
- `allocate_first_free_addr()`：返回首个空闲地址（1~64）。
- `register_uid_addr(uid, addr)`：将UID绑定到指定地址。
- `ensure_addr_for_uid(uid)`：若UID已注册则返回原地址，否则分配新地址并注册。

## 3.3 主循环与总线调度算法

### 3.3.1 时间驱动调度

网关主循环以`POLL_DELAY_MS = 10ms`为节拍运行，使用三个时间点控制调度：

- `nextPollAt`：下一次轮询已注册节点的时间，默认间隔200ms。
- `nextDiscoveryAt`：下一次发送发现广播的时间，默认间隔1000ms。
- `nextRs485TxAt`：下一次允许RS485发送的时间，默认间隔50ms（避免总线过于繁忙）。

### 3.3.2 发送优先级

在每个调度节拍中，RS485发送槽的优先级从高到低为：

1. **显式命令队列**（`rs485_cmd_msgq`）：来自串口屏或WiFi的`LOCK/GET`命令。
2. **发现探测**：若到达`nextDiscoveryAt`，发送`REQ,0,DISCOVER`。
3. **周期轮询**：若到达`nextPollAt`，发送`REQ,addr,GET`给下一个已注册节点。

每次成功发送后，将`nextRs485TxAt`推迟50ms，保证帧间隔。

### 3.3.3 轮询游标

轮询使用`pollCursor`记录当前轮询位置，调用`find_next_registered_addr(pollCursor)`查找下一个已注册节点。找到后发送GET并更新`pollCursor = (addr % 64) + 1`，实现循环轮询。

## 3.4 RS485帧处理流程

### 3.4.1 注册帧处理（REG）

- **上行`REG,REQ,UID=...`**：收到后调用`ensure_addr_for_uid()`分配地址，然后发送`SET,0,ADDR,UID,addr`给从机，同时向上游转发`REG,ASSIGN,addr,UID=...`。
- **上行`REG,ACK,addr,UID=...`**：收到后调用`register_uid_addr()`确认绑定，标记该节点`online=true`，并更新`lastSeen`。

### 3.4.2 数据帧处理（REPORT）

收到`REPORT,node_id,...`后：
1. 查找对应`NodeState`。
2. 标记`online=true`，更新`lastSeen`。
3. 调用`parse_telemetry()`逐字段解析`T=`、`H=`、`FD=`、`FA=`、`L=`等键值对，更新状态缓存。
4. 调用`forward_upstream(rawLine)`将原始行转发到串口屏与WiFi。
5. 调用`screen_publish_latest_snapshot(state)`更新串口屏上的温湿度、火焰风险、锁状态显示。

### 3.4.3 离线帧处理

网关本身不接收从机发送的`OFFLINE`帧（由网关主动判定），但在某些扩展场景下，若收到则从状态机中移除在线标记。

## 3.5 串口屏协议适配

### 3.5.1 Nextion指令格式

Nextion串口屏的指令以ASCII文本发送，以三个连续的`0xFF`字节作为结束码。网关封装了`screen_send_nextion_cmd()`函数：

```cpp
void screen_send_nextion_cmd(const char *cmd) {
    uart_send(screen, cmd);
    uart_poll_out(screen, 0xFF);
    uart_poll_out(screen, 0xFF);
    uart_poll_out(screen, 0xFF);
}
```

### 3.5.2 状态映射

- 温度：`home.T.txt = "25C"`
- 湿度：`home.H.txt = "55%"`
- 火焰数字：`home.FD.txt = "N"`（正常）或`"A"`（告警）
- 火焰风险：`home.FA.txt = "X%"`，根据`flameMv`线性映射：3300mV对应0%，0mV对应100%。
- 锁状态：`home.L.txt = "LCK"`（锁定）、`"ULCK"`（解锁）或`"-"`（未知）。

### 3.5.3 屏幕输入处理

屏幕发送的触摸事件经配置后可映射为ASCII文本帧：
- `LOCK,node_id,0/1`：本地锁定/解锁命令。
- `GET,node_id`：请求立即轮询某节点。
- `NETCFG,...` / `NETCLR` / `NETSTAT`：网络配置相关命令，直接透传给ESP8266。

## 3.6 WiFi输入处理与命令入队

### 3.6.1 ESP8266下行命令解析

ESP8266从MQTT收到云端命令后，通过UART发送给网关，格式为：

```text
CMD,<command_id>,LOCK,<node_id>,<0|1>
CMD,<command_id>,GET,<node_id>
```

网关的`process_wifi_line()`解析后调用`execute_gateway_command()`。

### 3.6.2 命令入队与ACK机制

`execute_gateway_command()`执行以下校验：
1. `node_id`是否有效（1~64）。
2. 该`node_id`是否已完成UID注册。
3. 对于`LOCK`，`value`是否为0或1；对于`GET`，无需value。

若校验通过，构造`Rs485Cmd`结构体并放入`rs485_cmd_msgq`。该结构体包含：
- `action`：`Get`或`Lock`
- `nodeId`、`value`
- `needAck`：是否需要向云端回执
- `commandId`：用于关联云端命令

若校验失败或队列满，立即通过WiFi UART发送`ACK,<command_id>,failed,<code>`。

当命令实际从RS485发出后，网关再次发送`ACK,<command_id>,sent,rs485_written`。

## 3.7 离线检测算法

主循环每次节拍都会遍历全部64个节点：

```cpp
for (int i = 0; i < NODE_ADDR_MAX; ++i) {
    if (states[i].online && (now - states[i].lastSeen > NODE_OFFLINE_TIMEOUT_MS)) {
        states[i].online = false;
        // 构造 OFFLINE,i+1 并转发上游
    }
}
```

`NODE_OFFLINE_TIMEOUT_MS`设置为15000ms（15秒）。判定离线后，向上游（串口屏+WiFi）广播`OFFLINE,node_id`，云端收到后将该节点标记为离线。

---

# 第四章 ESP8266通信模块设计与实现

## 4.1 固件架构

ESP8266固件基于Arduino框架编写，核心任务包括：
1. UART通信：与网关主机的协议交互。
2. 网络配置持久化：使用LittleFS保存WiFi与MQTT配置。
3. WiFi连接管理：自动重连、状态监控、断连原因记录。
4. MQTT客户端：基于PubSubClient，实现上行透传与下行命令接收。
5. 链路状态聚合：定期上报`LINK`综合状态帧。

## 4.2 网络配置持久化

### 4.2.1 配置文件格式

配置以纯文本行格式保存在LittleFS的`/netcfg.txt`中，共7行：
1. WiFi SSID
2. WiFi密码
3. MQTT主机地址
4. MQTT端口
5. MQTT Client ID
6. MQTT用户名
7. MQTT密码

```cpp
static bool save_config(const NetConfig* cfg) {
    File f = LittleFS.open(CONFIG_FILE_PATH, "w");
    f.printf("%s\n", cfg->ssid);
    // ... 共7行
    f.close();
}
```

### 4.2.2 字段校验

所有字段不允许包含逗号`,`、回车`\r`或换行`\n`。空密码与空MQTT用户名/密码是允许的，但SSID、MQTT主机、Client ID不能为空。

### 4.2.3 配置更新流程

网关通过UART发送`NETCFG,ssid,pass,host,port,client_id,user,pass`后，ESP8266解析并保存，回复`NETCFG,SAVED`，然后重置WiFi/MQTT连接管道以应用新配置。

## 4.3 WiFi连接状态机

### 4.3.1 自动重连机制

`wifi_begin_if_needed()`在以下条件下触发：
- 配置有效（`g_cfg.valid == true`）。
- WiFi未连接。
- 距离上次重试已超过`WIFI_RETRY_INTERVAL_MS`（5秒）。

每次重连前发送`WIFI,CONNECTING`状态帧。

### 4.3.2 连接超时保护

设置`WIFI_CONNECT_STALL_TIMEOUT_MS = 15s`。若`WiFi.begin()`后15秒内仍未连接，则标记为`CONNECT_TIMEOUT`并在下一个调度周期再次尝试。

### 4.3.3 事件回调

- `on_wifi_disconnect`：记录断开原因码（reason code），发送`LINK`状态帧。
- `on_wifi_got_ip`：清除原因码，发送带IP地址的`LINK`帧。

## 4.4 MQTT客户端实现

### 4.4.1 连接与订阅

在WiFi连接成功后，`mqtt_begin_if_needed()`尝试连接MQTT Broker。连接成功后：
1. 订阅命令主题`parking_lock/cloud/command`。
2. 发送`MQTT,CONNECTED`状态帧。
3. 发送`LINK`综合状态帧。

### 4.4.2 上行透传策略

ESP8266对从网关UART收到的以下帧不做解析，直接通过MQTT发布到`parking_lock/gateway/up/raw`：
- `REPORT,...`
- `OFFLINE,...`
- `ACK,...`
- `REG,...`

### 4.4.3 下行命令解析

MQTT消息回调`mqtt_callback`支持两种命令格式：

**文本格式**：
```text
CMD,<command_id>,LOCK,<node_id>,<0|1>
CMD,<command_id>,GET,<node_id>
```

**JSON格式**：
```json
{"command_id":"uuid","action":"LOCK","node_id":1,"value":1}
```

解析成功后，将命令转换为文本格式通过UART发送给网关，并立即向MQTT发布ACK：`ACK,<command_id>,sent,serial_written`。若解析失败，发布`ACK,<command_id>,failed,bad_command`。

## 4.5 链路状态聚合帧（LINK）

为简化网关对网络状态的感知，ESP8266将WiFi与MQTT状态聚合为单行`LINK`帧：

```text
LINK,<trigger>,WIFI,<wifi_status>,<ip_addr>,REASON,<reason_summary>,MQTT,<mqtt_status>
```

例如：
```text
LINK,periodic,WIFI,CONNECTED,192.168.1.20,REASON,OK,MQTT,CONNECTED
```

网关收到`LINK`帧后转发到串口屏，并调用`screen_publish_link_status()`更新WiFi/MQTT/服务器连接指示灯。

---

# 第五章 云端服务器设计与实现

## 5.1 技术选型与架构

云端服务采用Python FastAPI框架，异步IO（asyncio）驱动，核心模块包括：
- **MQTT客户端**（paho-mqtt）：接收网关上行数据与命令回执。
- **WebSocket管理器**（WsManager）：向前端广播实时快照。
- **内存存储**：节点状态表、事件循环队列、命令字典。
- **RESTful API**：提供节点查询、事件查询、命令创建与确认。
- **嵌入式Broker**（可选，amqtt）：当外部MQTT Broker不可用时自动启动。

## 5.2 数据模型

### 5.2.1 节点状态（NodeState）

```python
@dataclass
class NodeState:
    node_id: int
    online: bool = False
    temp: str = "-"
    humi: str = "-"
    flame_digital: str = "-"
    flame_analog: str = "-"
    lock_state: str = "-"
    uid: str = "-"
    reason: str = "-"
    last_seen: str = "-"
    last_raw: str = "-"
    updated_at: str = field(default_factory=utc_now)
```

系统默认预创建4个节点（node_id 1~4），支持动态扩展。

### 5.2.2 事件日志（EventItem）

使用`collections.deque`实现循环缓冲区，最大容量1500条。每条事件包含时间戳、级别（info/warn/error）、事件类型、原始帧内容、扩展字典。

### 5.2.3 命令记录（CommandItem）

```python
@dataclass
class CommandItem:
    id: str                # UUID
    created_at: str
    node_id: int
    action: str            # LOCK / GET
    value: int | None
    status: str            # pending / sent / done / failed
    ack_at: str | None
```

## 5.3 MQTT消息处理

### 5.3.1 上行数据主题

订阅`parking_lock/gateway/up/raw`，回调函数`on_mqtt_message`调用`handle_ingest_line()`进行解析：

- **`REPORT,node_id,reason,...`**：
  - 验证`node_id`存在。
  - 解析`T=`、`H=`、`FD=`、`FA=`、`L=`、`UID=`等键值对。
  - 更新`NODES[node_id]`的对应字段，设置`online=True`，更新`updated_at`。
  - 记录事件并触发WebSocket广播。

- **`OFFLINE,node_id`**：
  - 标记对应节点`online=False`，更新`updated_at`。
  - 记录warn级别事件并广播。

- **`ACK/ERR/BOOT/GATEWAY_READY`**：
  - 作为系统事件记录，用于网关状态监控。

### 5.3.2 命令回执主题

订阅`parking_lock/gateway/command_ack`，解析`ACK,<command_id>,<status>,<detail>`，更新`COMMANDS[command_id].status`。

**重要设计**：命令ACK不触发全局snapshot广播（避免前端将ACK误判为自然回包而消耗重试次数）。ACK仅更新命令状态表，前端通过判断`updated_at`是否变化来区分自然回包与ACK回执。

## 5.4 WebSocket实时推送

### 5.4.1 WsManager实现

使用`asyncio.Lock`保护客户端集合，提供：
- `connect(websocket)`：接受连接并加入集合。
- `disconnect(websocket)`：移除连接。
- `broadcast_json(payload)`：向所有存活客户端发送JSON。

### 5.4.2 快照（Snapshot）格式

```json
{
  "type": "snapshot",
  "nodes": [ /* NodeState列表 */ ],
  "events": [ /* 最近N条EventItem */ ],
  "mqtt_connected": true,
  "embedded_broker_started": false,
  "time": "2026-04-15T21:00:00+08:00"
}
```

客户端连接`/ws`后立即收到当前快照，之后任何状态变更都会收到增量广播（实际为完整快照覆盖）。

## 5.5 RESTful API设计

| 端点 | 方法 | 功能 |
|------|------|------|
| `/health` | GET | 健康检查，返回MQTT连接状态 |
| `/` | GET | 返回前端静态页面 `index.html` |
| `/ws` | WebSocket | 实时数据推送 |
| `/v1/ingest/line` | POST | 接收单条上行数据（HTTP fallback） |
| `/v1/ingest/batch` | POST | 批量接收上行数据 |
| `/v1/nodes` | GET | 查询所有节点状态 |
| `/v1/nodes/{node_id}` | GET | 查询单个节点状态 |
| `/v1/events` | GET | 查询最近事件日志 |
| `/v1/commands` | POST | 创建控制命令 |
| `/v1/commands/pending` | GET | 查询待处理命令 |
| `/v1/commands/{id}/ack` | POST | 手动确认命令执行结果 |

## 5.6 嵌入式MQTT Broker（可选）

启动时执行`start_embedded_broker_if_needed()`：
1. 检测外部Broker（`127.0.0.1:1883`）是否可连接（TCP 0.6s超时）。
2. 若不可连接且`amqtt`库已安装，则启动嵌入式Broker。
3. 随后FastAPI启动自身的MQTT客户端连接Broker。

这一定制机制使得系统在无外网、无外置Broker的实验环境中仍可独立运行。

---

# 第六章 前端Web UI设计与实现

## 6.1 技术架构

前端为单页应用（SPA），技术栈极简：
- **HTML5 + CSS3**：响应式布局，支持亮/暗主题。
- **原生JavaScript**：无框架依赖，减少构建复杂度，便于嵌入静态文件服务。
- **WebSocket API**：与云端建立`/ws`长连接，接收实时快照。
- **Fetch API**：用于HTTP命令提交与兜底刷新。

## 6.2 页面布局

- **顶部栏**：标题、主题切换按钮、告警音开关、手动刷新按钮、连接状态文本。
- **节点卡片区**：为每个在线节点渲染一张卡片，展示温度、湿度、锁状态、火焰数字/风险进度条、UID、更新时间。
- **事件日志区**：倒序展示最近系统事件（上报、离线、命令、ACK等）。

## 6.3 实时状态同步机制

### 6.3.1 WebSocket连接管理

```javascript
function connectWs() {
    ws = new WebSocket(wsUrl());
    ws.onopen = ...;
    ws.onmessage = (ev) => {
        const msg = JSON.parse(ev.data);
        if (msg.type === "snapshot") applySnapshot(msg);
    };
    ws.onclose = () => setTimeout(connectWs, 1500); // 自动重连
}
```

### 6.3.2 快照应用与变更检测

`applySnapshot()`接收完整快照后执行：
1. `renderNodes()`：重新渲染节点卡片（仅在线节点）。
2. `renderEvents()`：更新事件列表。
3. `checkPendingLocks()`：检查是否有正在等待的锁定/解锁操作需要重试。
4. `maybeAlarm()`：检查火焰告警并播放蜂鸣。

**变更检测**：前端维护`lastUpdatedAt`映射表，记录每个节点上一次的`updated_at`。当新快照中某节点的`updated_at`发生变化时，认为该节点收到了新的自然回包（REPORT/OFFLINE）。ACK等执行回执不会更新节点的`updated_at`，因此不会触发重试。

## 6.4 命令自动重试机制

### 6.4.1 问题背景

在RS485半双工总线环境中，云端发送的`LOCK`命令可能因总线忙、从机暂时离线或帧丢失而未能生效。若仅依赖用户手动重试，体验较差。

### 6.4.2 重试状态机

用户点击锁定/解锁后：
1. 前端在`pendingLocks`映射中记录该节点的预期锁状态与剩余重试次数（`LOCK_MAX_RETRIES = 20`）。
2. 立即发送`POST /v1/commands`创建命令。
3. 在后续每次收到WebSocket快照时，检查该节点的实际锁状态：
   - 若已达成预期状态 → 清除`pendingLocks`，提示成功。
   - 若未达成且该节点的`updated_at`有变化（收到自然回包）→ `retriesLeft--`，自动再次发送`LOCK`命令。
   - 若重试次数耗尽 → 提示失败并清除`pendingLocks`。

### 6.4.3 ACK过滤设计

网关向云端发送的`ACK,uuid,sent,rs485_written`仅更新命令状态表，不更新节点状态的`updated_at`。因此，即使ACK先到达，也不会被误判为自然回包而消耗重试次数。

## 6.5 火焰告警与声音提示

### 6.5.1 风险计算

前端根据`flame_analog`（mV）计算火焰风险百分比：

```javascript
function flameRiskPercent(valueText) {
    const v = Number(valueText);
    return clamp(Math.round(((3300 - v) / 3300) * 100), 0, 100);
}
```

3300mV对应0%风险，0mV对应100%风险。

### 6.5.2 告警条件

当满足以下任一条件时触发告警：
- `flame_digital === "0"`（数字量低电平告警）
- `risk >= 70%`

### 6.5.3 蜂鸣实现

使用Web Audio API生成880Hz方波，持续150ms，音量增益0.05。为了避免连续刺耳，设置2秒冷却间隔（`lastAlarmAt`）。

---

# 第七章 通信协议规范

## 7.1 RS485从机-网关协议

### 7.1.1 物理层与链路层

- **波特率**：115200 bps
- **数据位**：8，**停止位**：1，**校验**：无
- **拓扑**：半双工总线，一主多从（最多64从机）
- **帧格式**：ASCII文本，字段以逗号`,`分隔，以`\r\n`结束
- **总线控制**：主机主导发送，从机仅在收到针对本机的命令或广播后响应

### 7.1.2 命令帧定义

#### 主机请求帧

| 帧名称 | 格式 | 说明 |
|--------|------|------|
| 心跳请求 | `REQ,<node_id>,PING` | `node_id`为0时广播，各从机按地址slot延迟响应 |
| 数据请求 | `REQ,<node_id>,GET` | 要求从机立即上报传感器数据 |
| 锁定控制 | `SET,<node_id>,LOCK,<0\|1>` | 0=解锁，1=锁定 |
| 发现广播 | `REQ,0,DISCOVER` | 寻找未注册的从机（node_id=0） |
| 地址分配 | `SET,0,ADDR,<UID_HEX>,<addr>` | 为指定UID的从机分配地址 |

#### 从机响应帧

| 帧名称 | 格式 | 说明 |
|--------|------|------|
| PING响应 | `ACK,<node_id>,PONG` | 心跳确认 |
| 数据上报 | `REPORT,<node_id>,<reason>,T=...,H=...,FD=...,FA=...,L=...,DE=...,AE=...,SE=...,RB=...,RL=...,RC=...,RO=...,UID=...` | 完整状态报告 |
| 锁定确认 | `ACK,<node_id>,LOCK,<0\|1>` | 确认已执行锁定/解锁 |
| 注册请求 | `REG,REQ,UID=<UID_HEX>` | 未注册从机请求分配地址 |
| 注册确认 | `REG,ACK,<addr>,UID=<UID_HEX>` | 确认已接受分配地址 |

### 7.1.3 总线退避策略

从机在发送前必须等待总线空闲`≥4ms`。若总线忙，执行最多5次退避，退避时间计算：

```
backoff_ms = 1 + ((node_id + attempt * 3 + (now & 0x03)) % 7)
```

## 7.2 网关-ESP8266 UART协议

### 7.2.1 帧格式

与RS485协议相同：ASCII文本，逗号分隔，`\r\n`结束。

### 7.2.2 网关→ESP帧

| 类型 | 示例 | 说明 |
|------|------|------|
| 启动就绪 | `READY,GATEWAY,1` | 网关启动后发送 |
| 上行透传 | `REPORT,1,POLL,T=26.00,...` | 原始RS485帧透传 |
| 网络配置 | `NETCFG,SSID,PASS,HOST,PORT,CLIENT,USER,PASS` | 设置并保存网络参数 |
| 清除配置 | `NETCLR` | 删除持久化配置 |
| 查询状态 | `NETSTAT` | 请求ESP返回当前网络状态 |
| 命令回执 | `ACK,<cmd_id>,sent,rs485_written` | 网关对ESP下发命令的响应 |

### 7.2.3 ESP→网关帧

| 类型 | 示例 | 说明 |
|------|------|------|
| 启动就绪 | `READY,ESP,1` | ESP启动后发送 |
| 下行命令 | `CMD,uuid,LOCK,1,1` | 云端命令透传给网关 |
| 链路状态 | `LINK,trigger,WIFI,CONNECTED,192.168.1.1,REASON,OK,MQTT,CONNECTED` | 综合网络状态 |
| WiFi状态 | `WIFI,CONNECTED,192.168.1.1` | 单独WiFi状态 |
| MQTT状态 | `MQTT,CONNECTED` | 单独MQTT状态 |

## 7.3 MQTT主题规范

| 主题 | 方向 | 载荷内容 |
|------|------|----------|
| `parking_lock/gateway/up/raw` | 网关→云端 | 原始上行文本帧 |
| `parking_lock/cloud/command` | 云端→网关 | `CMD,uuid,LOCK/GET,...` |
| `parking_lock/gateway/command_ack` | 网关→云端 | `ACK,uuid,sent/failed,code` |

## 7.4 HTTP API规范

### 7.4.1 请求/响应格式

所有API使用JSON格式，Content-Type为`application/json`。

### 7.4.2 核心接口详细说明

#### POST /v1/ingest/line

**请求体**：
```json
{
  "line": "REPORT,1,POLL,T=26.00,H=55.00,FD=1,FA=3289,L=0,UID=12AB34CD"
}
```

**响应体**：
```json
{
  "accepted": true,
  "event": "report",
  "node_id": 1
}
```

#### POST /v1/commands

**请求体**：
```json
{
  "node_id": 1,
  "action": "LOCK",
  "value": 1
}
```

**响应体**：
```json
{
  "id": "uuid-string",
  "created_at": "2026-04-15T21:00:00+08:00",
  "node_id": 1,
  "action": "LOCK",
  "value": 1,
  "status": "sent",
  "ack_at": null
}
```

---

# 第八章 系统时序分析

## 8.1 节点注册时序

1. **T0**：从机上电，初始化完成，`node_id = 0`。
2. **T0~T1**：网关周期发送`REQ,0,DISCOVER`（周期1s）。
3. **T1**：从机收到`DISCOVER`，执行随机退避（2~14ms），发送`REG,REQ,UID=...`。
4. **T2**：网关收到注册请求，分配首个空闲地址（如1），发送`SET,0,ADDR,UID,1`。
5. **T3**：从机收到地址分配，验证UID匹配，设置`node_id=1`，回复`REG,ACK,1,UID=...`。
6. **T4**：网关收到`REG,ACK`，将UID与地址绑定，标记节点1为在线。

从机从启动到完成注册的最长时间约为1s（等待下一个DISCOVER）+ 处理延迟，平均约500ms。

## 8.2 周期轮询时序

- **轮询间隔**：`POLL_INTERVAL_MS = 200ms`
- **发送间隔保护**：`RS485_TX_INTERVAL_MS = 50ms`
- **最大注册节点数**：假设64个全部在线

轮询周期 = max(200ms, 64 × 50ms) = 3200ms。即每个节点约3.2秒被轮询一次。实际使用中节点数通常不超过10个，轮询周期约1秒。

## 8.3 远程控制时序

1. **T0**：前端用户点击“锁定”。
2. **T1**：`POST /v1/commands`到达FastAPI，生成UUID，MQTT发布`CMD,uuid,LOCK,1,1`。
3. **T2**：ESP8266收到MQTT命令，解析后UART发送给网关。
4. **T3**：网关收到`CMD`，校验并入队`rs485_cmd_msgq`，回复`ACK,uuid,sent,rs485_written`给ESP，ESP再MQTT发布ACK。
5. **T4**：网关在下个RS485发送槽（最晚50ms后）发送`SET,1,LOCK,1`。
6. **T5**：从机执行舵机动作，回复`ACK,1,LOCK,1`和`REPORT,1,SET,L=1,...`。
7. **T6**：网关转发REPORT到ESP，ESP MQTT publish到云端。
8. **T7**：云端解析REPORT，更新节点状态，WebSocket广播快照。
9. **T8**：前端收到快照，显示节点1为锁定状态。

端到端延迟（T0~T8）通常在几百毫秒到1.5秒之间，取决于网络条件与总线负载。

---

# 第九章 关键技术问题与解决方案

## 9.1 RS485半双工总线冲突问题

**问题**：多从机同时响应或从机与主机发送窗口重叠时，帧可能冲突。

**解决方案**：
1. 从机发送前严格检查`RS485_IDLE_GUARD_MS=4ms`空闲时间。
2. 冲突时执行随机退避（1~7ms），最多5次重试。
3. 网关通过`RS485_TX_INTERVAL_MS=50ms`控制发送节奏，避免连续发送。

## 9.2 DHT11读取受舵机干扰问题

**问题**：SG90舵机启动时电流突变导致电源纹波，DHT11偶发返回错误数据。

**解决方案**：从机维护`dht_temp_cache_centi`与`dht_humi_cache_centi`。当`read_dht_values()`失败时，若存在有效缓存则使用缓存值，避免上报异常尖峰。

## 9.3 前端命令自动重试与ACK过滤问题

**问题**：网关收到云端命令后会先发送ACK（表示已写入RS485），但ACK到达云端时从机可能尚未执行完毕。若前端将ACK误认为执行完成，会在锁状态未变时停止重试。

**解决方案**：云端在收到ACK时仅更新命令状态表的`status`，**不更新节点状态的`updated_at`**。前端仅通过`updated_at`变化判断是否收到自然回包（REPORT/OFFLINE），从而正确驱动重试逻辑。

## 9.4 节点离线判定与状态同步问题

**问题**：从机断电或通信故障后，云端与串口屏可能长时间显示过时的在线状态。

**解决方案**：
1. 网关维护15秒超时机制，未收到帧则广播`OFFLINE`。
2. 云端收到`OFFLINE`立即标记节点离线并广播。
3. 前端渲染节点卡片时仅显示`online=true`的节点，离线节点自动从卡片区消失，保留在事件日志中。

## 9.5 网络配置持久化与动态更新问题

**问题**：ESP8266断电后需要重新配置WiFi参数，现场维护成本高。

**解决方案**：ESP8266使用LittleFS将网络配置持久化到Flash。用户通过串口屏输入网络参数后，网关透传给ESP8266，ESP保存后自动重置连接管道并应用新配置，无需重新烧录固件。

---

# 第十章 结论

本文档详细阐述了一套基于RS485半双工总线、ESP8266物联网透传与FastAPI云端服务的智能车位锁监控系统的设计与实现。系统通过模块化的硬件分层与文本协议解耦，实现了从现场感知、本地控制到远程监控的完整闭环。关键技术创新点包括：基于UID的自动地址分配机制、多路UART并发调度与离线检测、云端-前端协同的ACK过滤与自动重试策略、以及ESP8266的链路状态聚合与配置持久化。该系统具有良好的可扩展性与可靠性，为地下车库等场景的智能化车位管理提供了可行的技术方案。

---

# 附录A 术语表

| 术语 | 说明 |
|------|------|
| RS485 | 一种差分信号串行通信标准，支持长距离、多节点、半双工通信 |
| Zephyr RTOS | 面向资源受限设备的开源实时操作系统 |
| MQTT | 消息队列遥测传输，轻量级发布/订阅协议 |
| FastAPI | 现代、高性能的Python Web框架 |
| WebSocket | 在单个TCP连接上进行全双工通信的协议 |
| LittleFS | 适用于Flash存储的轻量级文件系统 |
| Nextion | 带串口通信能力的HMI触摸屏 |
| UID | 唯一设备标识符（Unique Device Identifier） |
| ACK | 确认回执（Acknowledgement） |
| HMI | 人机界面（Human-Machine Interface） |

# 附录B 代码目录结构

```
app/
├── parking_lock_slave/          # 从机Zephyr应用
│   └── src/main.cpp
├── parking_lock_gateway/        # 网关Zephyr应用
│   ├── src/main.cpp
│   ├── ESP_UART_PROTOCOL.md
│   └── SCREEN_UART_PROTOCOL.md
├── parking_lock_espwifi/        # ESP8266 Arduino固件
│   └── main/main.ino
└── parking_lock_cloud/          # 云端服务
    ├── server.py
    ├── serial_mqtt_bridge.py
    ├── serial_http_bridge.py
    ├── run_embedded_server.py
    └── static/
        ├── index.html
        ├── app.js
        └── styles.css
```
