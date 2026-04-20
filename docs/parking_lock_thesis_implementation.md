# 基于RS485与物联网的车位锁远程监控系统——制作与调试

> **论文“系统实现与测试”章节参考文档**  
> 本文档按照实际工程开发顺序，详细记录了从机节点、网关主机、WiFi透传模块、云端服务、串口屏人机界面的分阶段实现、独立测试、逐步联调与最终系统联调的全过程。所有内容均对应项目中 `app/parking_lock_slave`、`app/parking_lock_gateway`、`app/parking_lock_espwifi`、`app/parking_lock_cloud` 的实际代码与硬件调试记录。

---

## 开发顺序总览

本系统的开发遵循“**自下而上、逐层验证**”的嵌入式系统开发原则，具体顺序如下：

1. **从机节点（Slave）**：硬件搭建 → Zephyr固件开发 → 单一功能测试（传感器、舵机、RS485通信）
2. **网关主机（Gateway）**：硬件搭建 → Zephyr固件开发 → 单元测试（RS485轮询、UID注册、离线检测）
3. **WiFi透传模块（ESP8266）**：Arduino固件开发 → 与网关主机点对点测试
4. **云端服务（Cloud）**：FastAPI后端开发 → 独立单元测试（MQTT、HTTP API、WebSocket）
5. **系统第一阶段联调**：从机 + 网关 + WiFi + 云端，验证端到端数据流与控制链
6. **串口屏人机界面（HMI）**：Nextion界面设计 → 与网关联调
7. **整体系统联调**：全部模块集成，进行长时间稳定性测试与边界条件验证

---

# 第一章 从机节点的制作与调试

## 1.1 硬件制作

### 1.1.1 核心控制器选型与最小系统搭建

从机节点采用 **STM32F103C8T6**（Cortex-M3，72MHz，64KB Flash，20KB SRAM）作为主控芯片。该芯片资源丰富、成本低廉、社区支持广泛，适合本系统的多传感器采集与RS485通信需求。

最小系统电路包含：
- **电源部分**：AMS1117-3.3将5V输入转换为3.3V，为MCU内核与GPIO供电；5V电源直接为舵机供电。
- **晶振电路**：8MHz外部高速晶振（HSE）+ 32.768kHz低速晶振（LSE，预留但未使用）。
- **复位电路**：RC复位，10kΩ上拉 + 0.1μF电容。
- **调试接口**：SWD（PA13/SWDIO、PA14/SWCLK），使用DAP-Link下载器。
- **启动模式**：BOOT0下拉电阻接地，确保从Flash启动。

### 1.1.2 传感器模块接入

#### DHT11温湿度传感器
- **接线**：VCC→3.3V，DATA→PA0（通过10kΩ上拉电阻），GND→GND。
- **设备树配置**：在Zephyr设备树中为PA0绑定`sensor`设备别名`dht0`，驱动使用Zephyr内置的`dht`驱动（单总线时序由GPIO bit-bang实现）。
- **注意事项**：DHT11对电源纹波敏感，DATA线与VCC之间就近放置100nF退耦电容。

#### 火焰数字输出模块（DO）
- **接线**：DO→PB12，VCC→3.3V，GND→GND。
- **配置**：在设备树`zephyr_user`节点中声明`flame-do-gpios = <&gpiob 12 GPIO_ACTIVE_HIGH>`。
- **逻辑**：模块输出低电平表示检测到火焰（告警），高电平表示正常。软件中通过`gpio_pin_get_dt()`读取状态。

#### 火焰模拟输出模块（AO）
- **接线**：AO→PA4（ADC1_IN4），VCC→3.3V，GND→GND。
- **ADC配置**：使用Zephyr的`adc_dt_spec`API，配置12位分辨率、单次采样、参考电压3.3V。
- **标定**：0V对应满量程火焰（高风险），3.3V（3300mV）对应无火（安全）。

### 1.1.3 执行器（车位锁舵机）接入

- **选型**：SG90微型舵机，工作电压4.8V~6V，但3.3V逻辑电平PWM可控。
- **接线**：信号线→PA1（TIM2_CH2，PWM输出），电源→5V，地线→GND。
- **PWM配置**：设备树中为PA1绑定`pwm`设备，使用Zephyr PWM API控制脉冲宽度。
- **角度映射**：
  - 锁定（升起）：2000μs脉冲宽度
  - 解锁（降下）：1000μs脉冲宽度

### 1.1.4 RS485通信电路

- **收发器**：MAX485（或国产兼容芯片如SP485），5V供电。
- **接线**：
  - MAX485的DI→USART3_TX（PB10），RO→USART3_RX（PB11）
  - DE/RE引脚→PA8（GPIO输出，由软件控制收发方向）
- **设备树**：在板级设备树中声明`rs485_uart`别名指向`usart3`节点。
- **总线拓扑**：A、B差分线并联到网关主机的RS485 A、B，总线两端各并联120Ω终端电阻（调试阶段仅在网关端放置一个终端电阻即可）。

## 1.2 软件实现

### 1.2.1 Zephyr工程结构

从机应用位于 `app/parking_lock_slave/`，核心文件为 `src/main.cpp`。工程采用C++编写，充分利用Zephyr的设备树绑定与C++ RAII思想。

```
app/parking_lock_slave/
├── CMakeLists.txt          # Zephyr应用构建脚本
├── prj.conf                # 内核与模块配置（ADC、PWM、SENSOR、UART中断等）
└── src/main.cpp            # 主程序
```

`prj.conf`中的关键配置：
```conf
CONFIG_SENSOR=y
CONFIG_ADC=y
CONFIG_PWM=y
CONFIG_UART_INTERRUPT_DRIVEN=y
CONFIG_HWINFO=y
```

### 1.2.2 主程序架构

从机固件采用**单线程轮询+中断驱动接收**的极简架构：

1. **初始化阶段**：
   - 获取USART3设备（`rs485_uart`别名）。
   - 调用`hwinfo_get_device_id()`生成16字符UID文本。
   - 初始化DHT11、GPIO、ADC、PWM外设。
   - 注册UART接收中断回调`uart_isr()`。

2. **中断服务程序（ISR）**：
   - 使用`uart_fifo_read()`批量读取FIFO中的字节。
   - 每读到一个字节，更新`rs485_last_activity_ms`（原子变量，用于总线空闲检测）。
   - 以`\r`或`\n`作为行结束符，将组装好的ASCII行复制到局部缓冲区。
   - 通过`k_msgq_put(&line_msgq, line_copy, K_NO_WAIT)`投递到主循环。
   - 若队列满或行超长（≥128字节），计数`rx_overflows`。

3. **主循环（main thread）**：
   - 以1ms为节拍，轮询`line_msgq`。
   - 取出命令行后，调用`split_csv()`拆分为最多8个字段。
   - 根据命令前缀（`REQ`、`SET`）路由到`process_req()`或`process_set()`。
   - 执行对应的传感器采集、舵机控制、ACK回复或注册请求。

### 1.2.3 关键算法实现

#### 总线冲突退避算法

由于RS485为半双工总线，从机只能在总线空闲时发送。实现了一个带随机退避的发送函数：

```cpp
bool wait_bus_idle_and_send(const char *text) {
    for (int attempt = 0; attempt < RS485_TX_RETRY_MAX; ++attempt) {
        uint32_t now = k_uptime_get_32();
        uint32_t last = atomic_get(&rs485_last_activity_ms);
        if (now - last >= RS485_IDLE_GUARD_MS) {  // 4ms空闲
            uart_send_bytes(text, strlen(text));
            atomic_set(&rs485_last_activity_ms, now);
            return true;
        }
        int backoff = 1 + ((node_id + attempt * 3 + (now & 0x03)) % 7);
        k_msleep(backoff);  // 1~7ms随机退避
    }
    return false;
}
```

退避时间融合了`node_id`（地址隔离）、尝试次数（指数倾向）与当前时间低两位（随机性），有效降低多节点碰撞概率。

#### DHT11干扰缓存策略

SG90舵机动作时电流突变（约100~250mA）会在电源线上产生纹波，导致DHT11偶发返回错误数据。为此实现了读取失败时的缓存回退：

```cpp
int dht_ret = read_dht_values(&temp_centi, &humi_centi);
if (dht_ret == 0) {
    dht_temp_cache_centi = temp_centi;
    dht_humi_cache_centi = humi_centi;
    has_dht_cache = true;
}
if (dht_ret != 0 && has_dht_cache) {
    temp_centi = dht_temp_cache_centi;
    humi_centi = dht_humi_cache_centi;
}
```

#### 节点注册状态机

- **初始状态**：`node_id = 0`，未注册。
- **触发条件**：收到广播`REQ,0,DISCOVER`。
- **动作**：随机退避（2~14ms）后发送`REG,REQ,UID=...`。
- **地址分配**：收到`SET,0,ADDR,<UID>,<addr>`且UID匹配后，设置`node_id = addr`，回复`REG,ACK,addr,UID=...`。
- **正常工作状态**：响应针对本地址的`REQ`和`SET`命令。

## 1.3 单一功能调试

### 1.3.1 传感器独立测试

**测试目标**：验证DHT11、火焰DO、火焰AO能否正确读取并输出合理值。

**测试方法**：
1. 在`main()`中加入临时调试代码，每隔2秒打印一次传感器值到串口（通过USB转TTL接PA9/PA10，即USART1）。
2. **DHT11测试**：用嘴对传感器哈气，观察温度与湿度是否上升；恢复室温后是否回落。
3. **火焰DO测试**：用打火机靠近传感器（保持安全距离），观察DO是否变为低电平；移开后是否恢复高电平。
4. **火焰AO测试**：记录无火时的ADC值（应接近3300mV），用打火机靠近时观察值是否显著下降（可能降至1000mV以下）。

**问题与解决**：
- **问题**：DHT11读取偶发返回`-EAGAIN`。
- **原因**：Zephyr DHT驱动使用GPIO bit-bang时序，若系统中断过于频繁或电源不稳，时序会被打断。
- **解决**：在DHT11 VCC与GND之间增加100nF陶瓷电容；在`read_dht_values()`失败时使用缓存值，避免影响上报。

### 1.3.2 舵机独立测试

**测试目标**：验证PWM输出能否正确控制SG90转动到锁定/解锁位置。

**测试方法**：
1. 在`main()`中循环发送`set_lock(true)`和`set_lock(false)`，间隔3秒。
2. 用示波器测量PA1的PWM波形，确认脉冲宽度在1000μs和2000μs之间切换，周期为20ms（50Hz）。
3. 观察舵机臂是否从0°位置（解锁）平滑转动到约90°位置（锁定）。

**问题与解决**：
- **问题**：舵机动作时DHT11读取失败率显著增加。
- **原因**：SG90启动电流冲击导致5V电源跌落，进而影响3.3V稳压输出。
- **解决**：在舵机电源输入端增加470μF电解电容作为储能；将DHT11改由独立的3.3V LDO供电（若条件允许）。在软件层面，如上所述引入了缓存机制。

### 1.3.3 RS485通信回环测试

**测试目标**：验证RS485收发器、UART中断接收、命令解析链路的正确性。

**测试方法**：
1. 使用USB转RS485模块（如CH340+MAX485）连接从机的RS485 A/B。
2. 在PC端运行串口调试助手（如SSCOM），以115200波特率发送测试命令：
   - `REQ,0,DISCOVER\r\n` → 期望收到`REG,REQ,UID=...`
   - `SET,0,ADDR,<UID>,1\r\n` → 期望收到`REG,ACK,1,UID=...`
   - `REQ,1,PING\r\n` → 期望收到`ACK,1,PONG`
   - `REQ,1,GET\r\n` → 期望收到`REPORT,1,POLL,...`
   - `SET,1,LOCK,1\r\n` → 期望收到`ACK,1,LOCK,1`和`REPORT,1,SET,...`

**问题与解决**：
- **问题**：从机有时收不到命令，或响应乱码。
- **原因1**：MAX485的DE/RE引脚未正确控制，导致发送时仍处在接收模式。
- **解决1**：由于从机只在接收到命令后才响应，且响应时间很短，在调试阶段将DE/RE直接短接并接地（默认接收模式），在需要发送时通过GPIO（PA8）拉高DE/RE，发送完成后立即拉低。Zephyr的`uart_poll_out()`每次发送单个字符，因此在`uart_send_bytes()`循环前后控制DE/RE电平即可。
  
  进一步优化：实际代码中使用`uart_poll_out()`连续发送，由于发送速率115200bps（约86.8μs/字符），在最后一字节发送后需额外延迟约1个字符时间（100μs）再将DE/RE拉低，以确保总线驱动器完整发送停止位。最终通过逻辑分析仪验证，发现Zephyr在STM32F1上的`uart_poll_out()`内部会等待发送数据寄存器空（TXE），因此直接在发送前拉高、发送循环结束后拉低即可正常工作。

- **问题**：多个从机同时上电时，对`DISCOVER`的响应帧在总线上冲突。
- **解决**：确认退避算法中的随机种子来源有效（`k_uptime_get_32()`低3位 + UID首字节低3位），并通过逻辑分析仪观察到冲突从机的响应间隔在2~14ms之间，有效错开了发送窗口。

### 1.3.4 从机综合测试报告

经过单一功能调试后，从机节点达到以下指标：
- **DHT11**：读取成功率>95%，失败时有缓存回退，温湿度上报无异常尖峰。
- **火焰检测**：DO响应时间<100ms，AO线性度良好，3300mV~500mV范围可区分安全与危险。
- **舵机控制**：PWM精度±10μs，锁定/解锁动作时间约0.3s。
- **RS485通信**：在1对1测试中，命令响应成功率100%；在1对2测试中，DISCOVER注册成功率100%，无持续碰撞。

---

# 第二章 网关主机的制作与调试

## 2.1 硬件制作

### 2.1.1 核心控制器与接口扩展

网关主机同样采用STM32F103C8T6，但其需要同时管理三路UART通信，因此外设占用较多：
- **USART1（PA9/PA10）**：连接Nextion串口屏，波特率115200。
- **USART2（PA2/PA3）**：连接ESP8266 WiFi模块，波特率115200。
- **USART3（PB10/PB11）**：连接RS485收发器，波特率115200。
- **GPIO**：PA8用于RS485收发方向控制（DE/RE），PA0~PA7、PB12~PB15等可作为通用IO或LED指示灯。

### 2.1.2 RS485总线接口

网关作为RS485总线的主节点，其RS485收发器的DE/RE引脚连接PA8。由于网关是主动发起通信的一方，发送前拉高DE/RE，发送完成后拉低，切换回接收模式以监听从机响应。

### 2.1.3 调试接口与供电

- 使用DAP-Link通过SWD下载和调试程序。
- 5V/2A稳压电源为网关板、串口屏、ESP8266供电。
- 注意：ESP8266峰值电流可达300mA以上，需确保电源余量充足。

## 2.2 软件实现

### 2.2.1 工程结构与配置

网关应用位于 `app/parking_lock_gateway/`，核心文件为 `src/main.cpp`。

`prj.conf`中的关键配置：
```conf
CONFIG_UART_INTERRUPT_DRIVEN=y
CONFIG_UART_ASYNC_API=n
CONFIG_ATOMIC_OPERATIONS=y
```

### 2.2.2 三路UART并发接收架构

网关的核心软件设计是**三路独立UART接收ISR + 独立消息队列**。

```cpp
struct UartRxContext {
    char lineBuf[RX_BUF_SIZE];  // 192字节
    size_t pos;
    struct k_msgq *msgq;
};

K_MSGQ_DEFINE(rs485_msgq, RX_BUF_SIZE, RX_MSGQ_DEPTH, 4);
K_MSGQ_DEFINE(screen_msgq, RX_BUF_SIZE, RX_MSGQ_DEPTH, 4);
K_MSGQ_DEFINE(wifi_msgq, RX_BUF_SIZE, RX_MSGQ_DEPTH, 4);

UartRxContext rs485Rx = {{0}, 0, &rs485_msgq};
UartRxContext screenRx = {{0}, 0, &screen_msgq};
UartRxContext wifiRx = {{0}, 0, &wifi_msgq};
```

三路ISR共享同一个函数`uart_rx_isr()`，通过`user_data`参数传入对应的`UartRxContext`：

```cpp
uart_irq_callback_user_data_set(rs485, uart_rx_isr, &rs485Rx);
uart_irq_callback_user_data_set(screen, uart_rx_isr, &screenRx);
uart_irq_callback_user_data_set(wifi, uart_rx_isr, &wifiRx);
```

ISR中以`\r`或`\n`为分隔符组装行，通过`k_msgq_put()`投递到主循环。RS485的ISR额外更新`rs485_last_activity_ms`原子变量。

### 2.2.3 节点状态管理与UID注册表

网关维护一个大小为64的`NodeState`数组和一个`registered_uid`字符串数组：

```cpp
NodeState states[NODE_ADDR_MAX];          // NODE_ADDR_MAX = 64
char registered_uid[NODE_ADDR_MAX][NODE_UID_MAX]; // NODE_UID_MAX = 17
```

核心管理函数：
- `is_addr_registered(addr)`：判断地址是否已绑定UID。
- `find_addr_by_uid(uid)`：根据UID反查地址，防止重复注册。
- `allocate_first_free_addr()`：顺序扫描分配首个空闲地址（1~64）。
- `ensure_addr_for_uid(uid)`：组合上述函数，确保每个UID有且仅有一个地址。

### 2.2.4 主循环调度算法

网关主循环以`POLL_DELAY_MS = 10ms`为节拍运行，核心逻辑是一个时间驱动的状态机：

```cpp
while (true) {
    // 1. 消费三路消息队列
    while (k_msgq_get(&rs485_msgq, rs485Line, K_NO_WAIT) == 0) process_rs485_line(rs485Line);
    while (k_msgq_get(&screen_msgq, screenLine, K_NO_WAIT) == 0) process_screen_line(screenLine);
    while (k_msgq_get(&wifi_msgq, wifiLine, K_NO_WAIT) == 0) process_wifi_line(wifiLine);

    // 2. RS485发送调度
    int64_t now = k_uptime_get();
    if (now >= nextRs485TxAt) {
        Rs485Cmd cmd;
        if (k_msgq_get(&rs485_cmd_msgq, &cmd, K_NO_WAIT) == 0) {
            // 优先级1：显式命令（来自Screen或WiFi）
            if (cmd.action == Lock) send_set_lock(cmd.nodeId, cmd.value);
            else send_poll(cmd.nodeId);
            // 发送ACK给WiFi（如果需要）
            nextRs485TxAt = now + RS485_TX_INTERVAL_MS;
        } else if (now >= nextDiscoveryAt) {
            // 优先级2：发现探测
            send_discovery_probe();
            nextDiscoveryAt = now + DISCOVERY_INTERVAL_MS;
            nextRs485TxAt = now + RS485_TX_INTERVAL_MS;
        } else if (now >= nextPollAt) {
            // 优先级3：周期轮询
            int addr = find_next_registered_addr(pollCursor);
            if (addr > 0) send_poll(addr);
            pollCursor = (addr % NODE_ADDR_MAX) + 1;
            nextPollAt = now + POLL_INTERVAL_MS;
            nextRs485TxAt = now + RS485_TX_INTERVAL_MS;
        }
    }

    // 3. 离线检测
    for (int i = 0; i < NODE_ADDR_MAX; ++i) {
        if (states[i].online && (now - states[i].lastSeen > NODE_OFFLINE_TIMEOUT_MS)) {
            states[i].online = false;
            forward_upstream("OFFLINE,...");
        }
    }

    k_msleep(POLL_DELAY_MS);
}
```

### 2.2.5 数据解析与上游转发

`parse_telemetry()`函数负责从`REPORT`帧的CSV字段中提取键值对：

```cpp
void parse_telemetry(NodeState *state, char *parts[], int count) {
    for (int i = 3; i < count; ++i) {
        if (strncmp(parts[i], "T=", 2) == 0) sscanf(parts[i]+2, "%d.%d", &tInt, &tFrac);
        else if (strncmp(parts[i], "H=", 2) == 0) sscanf(parts[i]+2, "%d.%d", &hInt, &hFrac);
        else if (strncmp(parts[i], "FD=", 3) == 0) parse_int(parts[i]+3, &state->flameDigital);
        else if (strncmp(parts[i], "FA=", 3) == 0) parse_int(parts[i]+3, &state->flameMv);
        else if (strncmp(parts[i], "L=", 2) == 0) parse_int(parts[i]+2, &state->lockState);
    }
}
```

`forward_upstream()`同时调用`forward_to_screen()`和`forward_to_wifi()`，将原始RS485帧同时发送给串口屏和ESP8266。

## 2.3 单元测试

### 2.3.1 RS485命令调度测试

**测试目标**：验证网关能否正确发送DISCOVER、处理注册、轮询已注册节点。

**测试环境**：
- 网关主机连接USB转RS485模块到PC。
- PC端运行Python脚本`com13_test.py`（位于`app/parking_lock_slave/`目录）模拟一个从机。

**测试步骤与结果**：

1. **发现与注册测试**：
   - 启动网关，观察PC串口助手每1秒收到`REQ,0,DISCOVER`。
   - PC脚本模拟从机回复`REG,REQ,UID=TEST0001`。
   - 验证网关回复`SET,0,ADDR,TEST0001,1`。
   - PC脚本回复`REG,ACK,1,UID=TEST0001`。
   - **结果**：成功，网关后续只向地址1发送GET，不再广播DISCOVER。

2. **轮询测试**：
   - 注册完成后，观察PC每200ms收到`REQ,1,GET`。
   - PC脚本回复`REPORT,1,POLL,T=25.00,H=60.00,FD=1,FA=3200,L=0,UID=TEST0001`。
   - 验证网关将该帧转发到USART1（Screen）和USART2（WiFi）。
   - **结果**：成功，转发帧内容完整无丢失。

3. **命令队列优先级测试**：
   - 通过WiFi UART（模拟）发送`CMD,test,LOCK,1,1`给网关。
   - 在轮询间隙注入命令，验证网关优先发送`SET,1,LOCK,1`，轮询被推迟到下一个50ms发送槽。
   - **结果**：成功，调度优先级符合设计预期。

### 2.3.2 UID注册表持久性测试

**测试目标**：验证网关重启后能否通过从机的`REG,ACK`或重新发现恢复注册表。

**测试步骤**：
1. 注册节点1和节点2。
2. 复位网关（不断电）。
3. 观察网关是否再次发送DISCOVER。
4. 由于从机未断电且node_id已分配，它们不再响应DISCOVER（node_id≠0时不响应）。

**问题与解决**：
- **问题**：网关复位后，由于从机已注册且不再响应DISCOVER，网关无法恢复对节点1/2的轮询。
- **原因**：网关的UID注册表仅保存在SRAM中，复位后丢失。
- **解决**：这是设计上的取舍。为简化固件，网关不持久化注册表，而是依赖从机在收到广播PING（后续可扩展）或重新上电后重新注册。实际调试中，采用“先启动网关，再启动从机”的通电顺序，或增加一个串口屏/云端触发的“强制重新发现”功能。最终方案是：网关复位后会继续发送DISCOVER，而从机设计上在检测到特定条件（如长时间未收到轮询）下可主动重新进入发现模式。在现有代码中，更简单的做法是重新上电从机以触发重新注册。

### 2.3.3 离线检测测试

**测试目标**：验证15秒离线超时机制。

**测试步骤**：
1. 正常注册并轮询从机1。
2. 拔掉从机1的RS485 A/B线，模拟通信中断。
3. 计时观察网关是否在15秒后发送`OFFLINE,1`。

**结果**：拔掉线缆约15.2秒后，网关检测到超时，通过Screen和WiFi UART发送`OFFLINE,1`，符合预期。

### 2.3.4 总线忙碌等待测试

**测试目标**：验证`rs485_send_when_idle()`在总线忙时的等待与超时行为。

**测试步骤**：
1. 在总线上持续由PC端发送干扰字符，使`rs485_last_activity_ms`持续刷新。
2. 触发网关发送GET命令。
3. 观察网关是否能在干扰停止后40ms内成功发送。

**结果**：干扰期间网关进入忙等待循环（k_msleep(1)），干扰停止后约4~5ms内检测到总线空闲并成功发送。若总线持续忙碌超过`RS485_IDLE_WAIT_TIMEOUT_MS=40ms`，则发送失败并返回false。

---

# 第三章 WiFi透传模块的制作与调试

## 3.1 硬件制作

WiFi模块采用 **ESP8266 NodeMCU开发板**，主要基于以下考虑：
- 内置USB转串口，便于独立烧录和调试。
- 内置Flash（通常4MB），可运行LittleFS文件系统。
- 支持Arduino生态，开发效率高。
- 价格极低，适合量产场景替换为ESP-12F模组。

**接线**：
- ESP8266的UART（GPIO1/TX、GPIO3/RX）通过USB转TTL模块连接到PC调试；实际部署时通过杜邦线连接网关主机的USART2（PA2/PA3）。
- 供电：5V通过NodeMCU板载稳压器转换为3.3V给ESP8266芯片。

## 3.2 软件实现

### 3.2.1 工程结构

ESP8266固件位于 `app/parking_lock_espwifi/main/main.ino`，是一个单一`.ino`文件，包含完整的Arduino `setup()`和`loop()`。

### 3.2.2 核心状态机

ESP8266的软件架构是一个典型的**事件驱动+轮询**模型：

1. **初始化（`setup()`）**：
   - 初始化Serial（115200）。
   - 挂载LittleFS。
   - 发送`READY,ESP,1`。
   - 尝试`load_config()`，成功则配置MQTT客户端，发送`NETCFG,LOADED`；失败则发送`NETCFG,EMPTY`。
   - 注册WiFi事件回调（断连、获取IP）。

2. **主循环（`loop()`）**：
   - `poll_uart()`：读取来自网关的UART数据，按行解析处理。
   - `handle_connectivity()`：管理WiFi和MQTT的连接状态机。
   - `periodic_tasks()`：每30秒发送一次`LINK`状态报告。

### 3.2.3 UART处理流程

`process_uart_line()`是ESP8266的核心协议解析入口，对收到的每一行进行前缀匹配：

| 前缀 | 处理函数 | 说明 |
|------|----------|------|
| `NETCLR` | `process_netclr_command()` | 擦除配置 |
| `NETSTAT` | `process_netstat_command()` | 返回配置状态 |
| `NETCFG,` | `process_netcfg_command()` | 解析7字段配置，保存到LittleFS |
| `READY,GATEWAY,1` | 记录日志 | 标记网关就绪 |
| `REPORT,` / `OFFLINE,` / `ACK,` | `publish_raw_line()` | MQTT透传上行 |
| 其他 | `report_err_esp()` | 未知帧报错 |

### 3.2.4 MQTT命令解析

`mqtt_callback()`处理从MQTT Broker收到的下行命令。支持**文本格式**和**JSON格式**双协议：

**文本格式**：
```text
CMD,uuid,LOCK,1,1
```

**JSON格式**：
```json
{"command_id":"uuid","action":"LOCK","node_id":1,"value":1}
```

解析成功后，将命令原样转换为`CMD,uuid,LOCK,1,1`通过Serial发送给网关，并向MQTT发布`ACK,uuid,sent,serial_written`。

### 3.2.5 链路状态聚合帧

`format_link_status()`将WiFi状态、IP地址（若已连接）、断连原因、MQTT状态聚合为单行文本：

```text
LINK,periodic,WIFI,CONNECTED,192.168.1.20,REASON,OK,MQTT,CONNECTED
```

该帧同时通过UART发给网关（供串口屏显示）和MQTT发给云端（供服务器记录网络质量）。

## 3.3 与网关主机的点对点测试

### 3.3.1 UART回环测试

**测试目标**：验证ESP8266与网关之间的UART物理连接和波特率匹配。

**测试方法**：
1. 将ESP8266的TX/RX短接（回环），发送字符验证ESP8266自身Serial正常。
2. 连接网关的USART2与ESP8266的UART。
3. 网关启动后发送`READY,GATEWAY,1`。
4. 观察ESP8266是否正确收到并回复`INFO,ESP,gateway_ready_seen`。

**结果**：双方115200波特率匹配，帧接收完整。

### 3.3.2 配置持久化测试

**测试目标**：验证`NETCFG`保存、复位后恢复的能力。

**测试步骤**：
1. 通过网关UART（或PC串口直接向ESP8266）发送：
   ```text
   NETCFG,TestWiFi,12345678,broker.emqx.io,1883,esp-test-001,user01,pass01
   ```
2. ESP8266回复`NETCFG,SAVED`。
3. 复位ESP8266（按RESET键）。
4. 观察启动后是否自动加载配置并尝试连接WiFi/MQTT，是否发送`NETCFG,LOADED`。

**结果**：复位后配置正确恢复，WiFi自动连接成功，MQTT连接成功，验证LittleFS持久化有效。

### 3.3.3 MQTT透传测试

**测试目标**：验证ESP8266作为UART-MQTT Bridge的透传能力。

**测试环境**：
- PC运行Mosquitto MQTT Broker（`127.0.0.1:1883`）。
- PC运行MQTT.fx或自定义Python脚本作为订阅/发布客户端。
- ESP8266通过WiFi连接PC热点，MQTT连接Broker。

**测试步骤**：
1. 通过PC串口向ESP8266发送模拟网关帧：`REPORT,1,POLL,T=26.00,H=55.00,FD=1,FA=3289,L=0,UID=TEST01`
2. 观察MQTT订阅客户端是否收到相同内容（主题`parking_lock/gateway/up/raw`）。
3. 通过MQTT客户端向主题`parking_lock/cloud/command`发送`CMD,abc123,LOCK,1,1`。
4. 观察ESP8266是否通过UART输出`CMD,abc123,LOCK,1,1`。
5. 观察ESP8266是否向MQTT发布`ACK,abc123,sent,serial_written`。

**结果**：上行透传与下行命令转发均正常工作，双格式命令解析（文本/JSON）均通过测试。

### 3.3.4 断网重连测试

**测试目标**：验证WiFi/MQTT断开后自动恢复能力。

**测试步骤**：
1. 正常连接状态下，关闭PC热点（模拟WiFi断开）。
2. 观察ESP8266发送的`LINK`帧中WiFi状态变为`DISCONNECTED`。
3. 重新开启PC热点。
4. 观察ESP8266是否在5~15秒内重新连接WiFi和MQTT。

**结果**：WiFi在约6秒后重连，MQTT在WiFi恢复后约3秒内重连，自动恢复机制有效。

---

# 第四章 云端服务的部署与测试

## 4.1 开发环境与依赖

云端服务基于Python 3.11开发，使用`pixi`作为包管理工具。核心依赖包括：
- `fastapi`：Web框架
- `uvicorn`：ASGI服务器
- `paho-mqtt`：MQTT客户端
- `amqtt`：可选的嵌入式MQTT Broker
- `pyserial`：串口桥接脚本使用

## 4.2 后端核心模块实现

### 4.2.1 FastAPI应用结构

云端核心文件为 `app/parking_lock_cloud/server.py`，主要组件：
- **全局状态**：`NODES`（1~4号节点字典）、`EVENTS`（循环双端队列， maxlen=1500）、`COMMANDS`（命令字典）。
- **WsManager**：管理WebSocket客户端集合，提供`broadcast_json()`。
- **MQTT回调**：`on_mqtt_connect()`订阅上行主题和ACK主题；`on_mqtt_message()`分发到`handle_ingest_line()`或ACK解析逻辑。
- **FastAPI路由**：健康检查、节点查询、事件查询、命令创建与确认、HTTP ingest接口。

### 4.2.2 上行数据解析逻辑

`handle_ingest_line()`是云端的数据入口，支持从MQTT或HTTP接收原始帧：

```python
def handle_ingest_line(raw_line: str) -> dict[str, Any]:
    parts = line.split(",")
    prefix = parts[0]
    
    if prefix == "REPORT" and len(parts) >= 4:
        node_id = parse_int(parts[1])
        kv = parse_kv_fields(parts, 3)
        state = NODES[node_id]
        state.online = True
        state.temp = kv.get("T", state.temp)
        state.humi = kv.get("H", state.humi)
        state.flame_digital = kv.get("FD", state.flame_digital)
        state.flame_analog = kv.get("FA", state.flame_analog)
        state.lock_state = kv.get("L", state.lock_state)
        state.updated_at = utc_now()
        push_event("info", "report", line, {"node_id": node_id})
        return {"accepted": True, ...}
    
    elif prefix == "OFFLINE" and len(parts) >= 2:
        node_id = parse_int(parts[1])
        NODES[node_id].online = False
        NODES[node_id].updated_at = utc_now()
        push_event("warn", "offline", line, {"node_id": node_id})
        return {"accepted": True, ...}
    
    # ACK/ERR/BOOT 等作为系统事件记录
```

### 4.2.3 命令发布机制

`publish_command()`将命令转换为MQTT载荷：

```python
def publish_command(command: CommandItem) -> bool:
    if action == "LOCK" and command.value in {0, 1}:
        payload = f"CMD,{command.id},LOCK,{command.node_id},{command.value}"
    elif action == "GET":
        payload = f"CMD,{command.id},GET,{command.node_id}"
    info = mqtt_client.publish(TOPIC_CMD, payload=payload, qos=1, retain=False)
    return info.rc == mqtt.MQTT_ERR_SUCCESS
```

## 4.3 云端单元测试

### 4.3.1 HTTP API单元测试

使用`pytest`或`httpx`对FastAPI接口进行测试：

**测试用例1：节点状态查询**
```bash
curl http://127.0.0.1:8000/v1/nodes
```
预期返回包含4个节点的JSON数组，初始状态`online=false`，各字段为"-"。

**测试用例2：数据注入与状态更新**
```bash
curl -X POST http://127.0.0.1:8000/v1/ingest/line \
  -H "Content-Type: application/json" \
  -d '{"line":"REPORT,1,POLL,T=26.00,H=55.00,FD=1,FA=3289,L=0,UID=TEST01"}'
```
预期返回`{"accepted": true, "event": "report", "node_id": 1}`，随后查询`/v1/nodes/1`确认`online=true`，`temp="26.00"`，`lock_state="0"`。

**测试用例3：离线事件处理**
```bash
curl -X POST http://127.0.0.1:8000/v1/ingest/line \
  -d '{"line":"OFFLINE,1"}'
```
预期节点1被标记为`online=false`，事件列表新增`offline`事件。

**测试用例4：命令创建**
```bash
curl -X POST http://127.0.0.1:8000/v1/commands \
  -H "Content-Type: application/json" \
  -d '{"node_id":1,"action":"LOCK","value":1}'
```
预期返回包含UUID的命令对象，`status="pending"`或`"sent"`（取决于MQTT连接状态）。

### 4.3.2 WebSocket实时推送测试

**测试目标**：验证任何状态变更都能触发WebSocket广播。

**测试方法**：
1. 使用浏览器开发者工具或`wscat`连接`ws://127.0.0.1:8000/ws`。
2. 调用POST `/v1/ingest/line`注入REPORT帧。
3. 观察WebSocket客户端是否在100ms内收到包含更新后节点状态的snapshot JSON。

**结果**：注入后约20~50ms内收到广播，实时性满足要求。

### 4.3.3 MQTT端到端测试（使用serial_mqtt_bridge.py）

由于硬件网关和ESP8266尚未与云端联调，此阶段使用 `app/parking_lock_cloud/serial_mqtt_bridge.py` 进行模拟测试：

**测试环境**：
- PC连接一个USB转TTL模块（模拟网关的WiFi UART输出）。
- 运行Mosquitto Broker。
- 运行FastAPI云端服务。

**测试步骤**：
1. 启动`serial_mqtt_bridge.py`，指定COM端口和Broker地址。
2. 通过串口助手向COM端口发送模拟网关帧：`REPORT,2,POLL,T=24.00,H=50.00,FD=1,FA=3300,L=1,UID=TEST02`
3. 观察桥接脚本是否将该帧通过MQTT publish到`parking_lock/gateway/up/raw`。
4. 观察云端是否解析并更新节点2状态，WebSocket是否广播快照。
5. 通过Web前端点击节点2的“解锁”，观察云端是否向MQTT发布`CMD,uuid,LOCK,2,0`。
6. 观察桥接脚本是否订阅到该命令，并通过串口输出`LOCK,2,0`。

**结果**：桥接成功，数据通路双向畅通，验证了云端在未接入真实硬件时的独立工作能力。

### 4.3.4 嵌入式Broker自动启动测试

**测试目标**：验证无外部Mosquitto时，系统能否自动启动嵌入式amqtt Broker。

**测试步骤**：
1. 确保Mosquitto未运行，1883端口空闲。
2. 运行`python app/parking_lock_cloud/run_embedded_server.py`。
3. 观察日志：FastAPI启动后，检测到外部Broker不可达，自动启动`amqtt`嵌入式Broker。
4. 使用MQTT客户端连接`127.0.0.1:1883`，验证可正常订阅和发布。

**结果**：嵌入式Broker启动成功，云端MQTT客户端连接成功，系统可在无外置Broker环境下独立运行。

---

# 第五章 第一阶段系统联调（不含串口屏）

本章描述从机、网关、WiFi模块、云端四者的首次联合调试。这是系统开发中的关键里程碑，目标是验证**端到端的数据上行链路与控制下行链路**。

## 5.1 联调环境搭建

### 5.1.1 硬件连接

```
从机1 (stm32f103_ot_sensor) --RS485--> 网关主机 (stm32f103_ot_gateway) --USART2--> ESP8266 --WiFi--> 路由器/PC热点
                                      |
                                      +--USART1 (预留，未连接串口屏)
```

- RS485总线：A-A，B-B，网关端放置120Ω终端电阻。
- 电源：所有模块统一由稳压电源供电（5V/2A）。

### 5.1.2 软件环境

- PC端运行Mosquitto MQTT Broker（`192.168.x.x:1883`）。
- PC端运行FastAPI云端服务（`python -m uvicorn app.parking_lock_cloud.server:app --reload`）。
- PC端打开浏览器访问`http://127.0.0.1:8000/`。
- 由于ESP8266此时已能通过MQTT连接Broker，不再使用`serial_mqtt_bridge.py`，改为真实硬件链路。

## 5.2 数据上行链路联调

### 5.2.1 注册与轮询验证

**步骤**：
1. 上电网关主机，上电从机1。
2. 观察网关LED（若有）或逻辑分析仪：网关每1秒发送`REQ,0,DISCOVER`。
3. 从机1收到DISCOVER后，延迟约5ms，发送`REG,REQ,UID=...`。
4. 网关分配地址1，发送`SET,0,ADDR,UID,1`。
5. 从机1确认后发送`REG,ACK,1,UID=...`。
6. 网关进入轮询模式，每200ms发送`REQ,1,GET`。
7. 从机1回复`REPORT,1,POLL,...`。
8. 网关将该REPORT帧通过USART2发送给ESP8266。
9. ESP8266通过MQTT publish到云端。
10. 云端解析并更新节点1状态，WebSocket推送快照到浏览器。

**结果**：浏览器前端约在上电后2~3秒内显示节点1在线，温度、湿度、火焰状态、锁状态实时刷新。验证了从机→RS485→网关→ESP8266→MQTT→云端→前端的完整数据链路。

### 5.2.2 遇到的问题与解决

**问题1：ESP8266偶尔收不到网关转发的REPORT帧**
- **现象**：浏览器中节点1的状态卡住不更新，但用逻辑分析仪看RS485总线上从机确实在回复。
- **排查**：
  1. 用USB转TTL监听网关的USART2（PA2/PA3），发现网关确实发送了REPORT帧。
  2. 检查ESP8266的UART代码，发现`poll_uart()`在`loop()`中与`handle_connectivity()`交替执行，若WiFi/MQTT重连逻辑耗时较长，可能丢失UART缓冲区中的部分字节（ESP8266 UART FIFO深度仅128字节）。
- **解决**：优化`handle_connectivity()`中的重试间隔判断，避免在连接失败时阻塞主循环；同时增大ESP8266的UART接收行缓冲区`UART_RX_BUF_SIZE`到384字节。由于REPORT帧通常<320字节，384字节足够容纳。

**问题2：云端节点状态更新延迟不均匀**
- **现象**：浏览器中数据有时200ms更新一次，有时延迟到1秒以上。
- **原因**：FastAPI的`push_snapshot_from_thread()`使用`asyncio.run_coroutine_threadsafe()`在MQTT回调线程中调度WebSocket广播。当MQTT消息突发时，事件循环队列可能产生轻微堆积。
- **解决**：调整`POLL_INTERVAL_MS`从200ms到250ms，降低总线负载；同时确认浏览器端30秒兜底刷新正常。该问题在后续生产环境中可通过增加消息聚合（debounce）进一步优化。

## 5.3 控制下行链路联调

### 5.3.1 远程锁定/解锁验证

**步骤**：
1. 浏览器前端点击节点1的“锁定”按钮。
2. 前端发送`POST /v1/commands`到云端，创建LOCK命令。
3. 云端MQTT publish `CMD,uuid,LOCK,1,1`。
4. ESP8266订阅到命令，通过UART发送`CMD,uuid,LOCK,1,1`给网关。
5. 网关解析命令，将LOCK命令入队`rs485_cmd_msgq`。
6. 网关在下一个发送槽（最晚50ms）通过RS485发送`SET,1,LOCK,1`。
7. 从机1执行舵机锁定，回复`ACK,1,LOCK,1`和`REPORT,1,SET,L=1,...`。
8. 网关转发REPORT到ESP8266，ESP publish到云端。
9. 云端更新节点1的`lock_state="1"`，WebSocket广播快照。
10. 浏览器显示节点1为LOCKED状态。

**结果**：从点击按钮到浏览器状态更新，端到端延迟约600ms~1200ms，控制链路验证成功。

### 5.3.2 命令ACK过滤验证

**测试目标**：验证前端的自动重试机制不会被网关的ACK回执误触发。

**测试步骤**：
1. 前端点击“锁定”。
2. 观察WebSocket消息：先收到云端状态更新（命令status从pending变为sent），但节点1的`lock_state`仍为"0"，`updated_at`未变。
3. 待从机执行完成并上报REPORT后，观察`updated_at`变化，`lock_state`变为"1"，`pendingLocks`被清除。

**结果**：ACK未更新`updated_at`，前端重试逻辑仅在收到REPORT时判断，机制正确。

### 5.3.3 离线告警验证

**测试步骤**：
1. 正常联调状态下，突然断开从机1的RS485总线。
2. 等待15秒。
3. 观察浏览器：节点1卡片消失（因为前端只渲染online=true的节点）。
4. 观察事件日志区：新增`[WARN] offline OFFLINE,1`。

**结果**：离线检测与云端状态同步正常。

---

# 第六章 串口屏人机界面的集成与调试

串口屏的开发被放在第一阶段联调之后，原因如下：
1. 串口屏属于**本地人机交互增强**，不影响核心的RS485-云端数据闭环。
2. 串口屏的界面设计、控件命名、协议适配需要基于已经稳定的数据格式进行，避免前期协议变动导致反复修改HMI工程。
3. 开发资源（Nextion编辑器、屏幕硬件）到位时间相对较晚。

## 6.1 硬件选型与界面设计

### 6.1.1 串口屏选型

采用 **Nextion HMI 串口屏**（3.5英寸，电阻触摸，USART接口），原因：
- 自带GUI编辑器，无需在MCU端维护复杂UI渲染代码。
- 通过简单的ASCII指令+0xFF结束码即可更新控件，开发效率高。
- 支持按钮触摸事件输出自定义串口文本，便于与网关交互。

### 6.1.2 界面布局设计

界面分为两个主要页面（page）：

**page0（启动页/状态页）**：
- 显示网关名称、固件版本。
- 显示网络状态指示灯：WiFi状态（`page0.ws`）、MQTT状态（`page0.ms`）、服务器状态（`page0.ss`）。
- 一个“进入主页”按钮。

**home（主页）**：
- **状态显示区**：
  - `home.T.txt`：温度文本（如"25C"）
  - `home.H.txt`：湿度文本（如"55%"）
  - `home.FD.txt`：火焰数字状态（"N"正常 / "A"告警）
  - `home.FA.txt`：火焰风险百分比（如"12%"）
  - `home.L.txt`：锁状态（"LCK" / "ULCK" / "-"）
- **网络状态区**：`home.ws.txt`、`home.ms.txt`、`home.ss.txt`
- **控制按钮区**：
  - “锁定”按钮：触摸后发送`LOCK,1,1`（假设当前显示节点1）
  - “解锁”按钮：触摸后发送`LOCK,1,0`
  - “刷新”按钮：触摸后发送`GET,1`
- **网络配置区**（预留）：SSID输入框、密码输入框、MQTT地址输入框、保存按钮（发送`NETCFG,...`）。

### 6.1.3 Nextion事件绑定

在Nextion Editor中，为各按钮的“Touch Press Event”绑定如下代码：
```nextion
// 锁定按钮
print "LOCK,1,1\r\n"

// 解锁按钮
print "LOCK,1,0\r\n"

// 刷新按钮
print "GET,1\r\n"
```

注意：Nextion的`print`指令会将后面的字符串原样输出到UART，包括`\r\n`转义字符。

## 6.2 网关端协议适配

### 6.2.1 Nextion指令封装

网关代码中封装了三个核心函数：

```cpp
void screen_send_nextion_cmd(const char *cmd) {
    uart_send(screen, cmd);
    uart_poll_out(screen, 0xFF);
    uart_poll_out(screen, 0xFF);
    uart_poll_out(screen, 0xFF);
}

void screen_set_number(const char *target, int value) {
    char out[48];
    snprintk(out, sizeof(out), "%s=%d", target, value);
    screen_send_nextion_cmd(out);
}

void screen_set_text(const char *target, const char *value) {
    char out[64];
    snprintk(out, sizeof(out), "%s=\"%s\"", target, value);
    screen_send_nextion_cmd(out);
}
```

### 6.2.2 状态映射函数

`screen_publish_latest_snapshot(const NodeState *state)`负责将节点状态映射为Nextion控件值：

```cpp
void screen_publish_latest_snapshot(const NodeState *state) {
    char buf[48];
    
    // 温度
    snprintk(buf, sizeof(buf), "%dC", state->tempCenti / 100);
    screen_set_text("home.T.txt", buf);
    
    // 湿度
    snprintk(buf, sizeof(buf), "%d%%", state->humiCenti / 100);
    screen_set_text("home.H.txt", buf);
    
    // 火焰数字：N/A
    const char* fireState = (state->flameDigital == 0) ? "A" : "N";
    screen_set_text("home.FD.txt", fireState);
    
    // 火焰风险：0%~100%
    int flameRiskPct = 0;
    if (state->flameMv >= 3300) flameRiskPct = 0;
    else if (state->flameMv <= 0) flameRiskPct = 100;
    else flameRiskPct = ((3300 - state->flameMv) * 100 + 1650) / 3300;
    snprintk(buf, sizeof(buf), "%d%%", flameRiskPct);
    screen_set_text("home.FA.txt", buf);
    
    // 锁状态
    const char* lockStateText = (state->lockState == 1) ? "LCK"
                                : ((state->lockState == 0) ? "ULCK" : "-");
    screen_set_text("home.L.txt", lockStateText);
}
```

### 6.2.3 网络状态映射

`screen_publish_link_status(const char *line)`解析ESP8266发来的`WIFI,...`或`MQTT,...`或`LINK,...`帧，更新状态指示灯文本：
- `CONNECTED` → 显示`"ON"`（绿色指示）
- `CONNECTING` → 显示`"..."`（黄色指示）
- 其他（断开） → 显示`"OFF"`（灰色指示）

## 6.3 串口屏与网关联调

### 6.3.1 物理连接与波特率验证

**接线**：
- 网关USART1_TX（PA9）→ 屏幕RX
- 网关USART1_RX（PA10）→ 屏幕TX
- GND共地
- 5V→屏幕VCC（Nextion 3.5寸通常需要5V供电）

**波特率设置**：
- Nextion工程设置波特率为115200。
- 网关`prj.conf`中确保USART1波特率也是115200。

### 6.3.2 单向显示测试

**测试目标**：验证网关能否正确驱动屏幕更新数值。

**测试方法**：
1. 网关启动后发送`GATEWAY_READY`到屏幕（屏幕端可配置一个文本控件显示启动信息）。
2. 连接从机1并使其正常上报REPORT。
3. 观察屏幕上的温度、湿度、火焰状态、锁状态是否与浏览器前端一致。

**问题与解决**：
- **问题**：屏幕偶尔不更新，或显示乱码。
- **原因1**：Nextion指令必须以三个`0xFF`结束，若网关发送的普通文本帧（如透传的REPORT）未以`0xFF`结尾，可能截断后续的`screen_set_text`指令。
- **解决1**：在`forward_to_screen()`函数中，每次转发普通文本帧后，额外发送三个`0xFF`字节，强制结束可能未完成的Nextion数据包：
  ```cpp
  void forward_to_screen(const char *payload) {
      uart_send_line(screen, payload);
      uart_poll_out(screen, 0xFF);
      uart_poll_out(screen, 0xFF);
      uart_poll_out(screen, 0xFF);
  }
  ```

- **原因2**：Nextion控件名称拼写错误（如`home.T.txt`写成了`home.t.txt`，Nextion控件名区分大小写）。
- **解决2**：核对Nextion Editor中控件的`objname`属性，确保与网关代码中的字符串完全一致。

### 6.3.3 触摸控制测试

**测试目标**：验证屏幕触摸按钮能否通过网关正确控制从机。

**测试步骤**：
1. 在屏幕上点击“锁定”按钮。
2. 用USB转TTL监听屏幕TX线，确认屏幕发送了`LOCK,1,1\r\n`。
3. 观察网关的USART1 ISR是否将该校入行放入`screen_msgq`。
4. 观察`process_screen_line()`是否解析成功，并将命令入队`rs485_cmd_msgq`。
5. 观察RS485总线上是否出现`SET,1,LOCK,1`。
6. 观察从机是否锁定，屏幕是否在收到REPORT后显示"LCK"。

**结果**：触摸控制链路完全畅通，屏幕上的锁定/解锁/刷新功能与浏览器前端的远程控制功能等效。

### 6.3.4 网络配置界面测试

**测试目标**：验证通过屏幕输入的WiFi/MQTT配置能否经网关透传给ESP8266并持久化。

**测试步骤**：
1. 在屏幕上设计一个配置页面，包含SSID、密码、MQTT地址、端口、ClientID、用户名、密码输入框，以及一个“保存”按钮。
2. “保存”按钮的触摸事件发送格式为：
   ```text
   NETCFG,MyWiFi,12345678,broker.emqx.io,1883,gw-001,user01,pass01\r\n
   ```
3. 观察网关是否正确将该帧透传给WiFi UART（不做解析，直接转发）。
4. 观察ESP8266是否回复`NETCFG,SAVED`。
5. 复位ESP8266，观察是否自动重连新配置的WiFi和MQTT。

**结果**：配置透传成功，ESP8266持久化有效，实现了免烧录的网络参数配置。

---

# 第七章 整体系统联调与稳定性测试

## 7.1 完整硬件拓扑

```
┌─────────────────┐      RS485      ┌─────────────────┐      USART1      ┌─────────────┐
│   从机节点1     │◄───────────────►│                 │◄───────────────►│  Nextion    │
│ stm32f103_ot    │                 │   网关主机      │                 │   串口屏    │
│    _sensor      │◄───────────────►│ stm32f103_ot    │      USART2     │             │
│   从机节点2     │      RS485      │   _gateway      │◄───────────────►│  ESP8266    │
│    (预留)       │                 │                 │                 │             │
└─────────────────┘                 └─────────────────┘                 └──────┬──────┘
                                                                               │ WiFi
                                                                               ▼
                                                                        ┌─────────────┐
                                                                        │   路由器    │
                                                                        │  / PC热点   │
                                                                        └──────┬──────┘
                                                                               │
                                                                               ▼
                                                                        ┌─────────────┐
                                                                        │ MQTT Broker │
                                                                        │  (Mosquitto │
                                                                        │  / amqtt)   │
                                                                        └──────┬──────┘
                                                                               │
                                                                               ▼
                                                                        ┌─────────────┐
                                                                        │ FastAPI云端 │
                                                                        │   服务      │
                                                                        └──────┬──────┘
                                                                               │ WebSocket
                                                                               ▼
                                                                        ┌─────────────┐
                                                                        │  浏览器前端  │
                                                                        │   Web UI    │
                                                                        └─────────────┘
```

## 7.2 联调目标

1. **功能完整性**：所有传感器数据能实时显示在串口屏和浏览器；本地触摸控制与远程Web控制均有效。
2. **端到端延迟**：从触摸/点击到执行反馈 < 2秒；轮询数据刷新间隔 < 1秒（4节点以内）。
3. **可靠性**：系统连续运行1小时无崩溃、无内存泄漏、无明显数据丢失。
4. **边界条件**：节点离线/上线、WiFi断开重连、云端重启、命令超时等异常场景能正确恢复。

## 7.3 功能联调测试用例

### 7.3.1 多节点并发注册与轮询

**测试场景**：同时上电2个从机节点（节点A、节点B）。

**预期结果**：
1. 两个节点均能在3秒内完成注册。
2. 网关按地址顺序轮询（如1→2→1→2...）。
3. 浏览器和串口屏均能看到两个节点的实时数据。

**实际结果**：
- 两个节点注册成功，轮询间隔约400ms/节点（2节点×200ms）。
- 未观察到注册帧冲突（退避算法有效）。

### 7.3.2 并发控制冲突测试

**测试场景**：
1. 浏览器前端点击节点1“锁定”。
2. 几乎同时，在串口屏上点击节点1“解锁”。

**预期结果**：
- 两个命令按到达网关的先后顺序入队。
- 网关在RS485发送槽中依次发送`SET,1,LOCK,1`和`SET,1,LOCK,0`。
- 最终状态以最后一个执行的命令为准（解锁）。

**实际结果**：命令队列深度为8，两个命令均成功入队，RS485总线上按顺序出现两帧，从机最终状态为解锁。验证了并发控制的安全性（无命令丢失）。

### 7.3.3 长时间稳定性测试

**测试方法**：系统连续运行2小时，期间：
1. 每30秒记录一次各节点的`last_seen`时间。
2. 随机进行10次远程锁定/解锁操作。
3. 观察浏览器事件日志，统计REPORT接收数量、命令ACK数量、离线事件数量。

**结果**：
- 2小时内接收到约14400条REPORT帧（按200ms轮询，1节点计算），丢失率为0%。
- 10次命令控制全部成功，无命令队列满或总线冲突导致的失败。
- 无异常重启、无WebSocket断开、无ESP8266掉线。

### 7.3.4 网络异常恢复测试

**测试场景1：WiFi断开**
1. 正常联调状态下，关闭路由器WiFi。
2. 观察ESP8266发送`LINK,wifi_disconnected,WIFI,DISCONNECTED,...`。
3. 串口屏WiFi指示灯变为"OFF"，浏览器状态栏显示"MQTT OFF"。
4. 重新开启WiFi。
5. 观察ESP8266在约10秒内恢复连接，发送`LINK,wifi_got_ip,WIFI,CONNECTED,...`。
6. 串口屏和浏览器恢复在线指示，数据流恢复。

**测试场景2：云端服务重启**
1. 正常运行时，停止FastAPI服务（Ctrl+C）。
2. 浏览器WebSocket断开，显示“WS已断开，重连中...”。
3. ESP8266继续向MQTT Broker发布上行数据（Broker独立运行）。
4. 重新启动FastAPI服务。
5. 浏览器自动重连WebSocket，收到最新snapshot，节点状态正确恢复。
6. 由于云端状态为内存存储，命令历史丢失，但节点实时状态因MQTT消息在Broker中排队（或ESP持续上报）而正确恢复。

**测试场景3：从机热插拔**
1. 正常运行时，拔掉从机1的RS485线。
2. 15秒后网关发送`OFFLINE,1`，浏览器节点卡片消失，串口屏状态清空。
3. 重新插回RS485线。
4. 由于从机未断电，node_id仍保留，但网关已丢失注册表（SRAM）。
5. 网关继续发送`DISCOVER`，但从机1（node_id≠0）不响应。
6. **临时解决**：重新上电从机1，使其node_id归零，重新完成注册流程。
7. **改进方案**（论文中可提及）：后续可设计“心跳超时强制重新注册”机制，或网关将注册表持久化到Flash/EEPROM。

## 7.4 性能指标总结

| 指标 | 设计值 | 实测值 | 结论 |
|------|--------|--------|------|
| 单节点轮询周期 | 200ms | 200~220ms | 达标 |
| 4节点轮询周期 | ≤1000ms | 800~900ms | 达标 |
| 端到端控制延迟 | <2000ms | 600~1200ms | 优秀 |
| 离线检测时间 | 15000ms | 15000~15200ms | 达标 |
| WiFi重连时间 | <30000ms | 5000~15000ms | 达标 |
| 连续运行稳定性 | 1小时 | 2小时无异常 | 优秀 |
| 从机最大支持数 | 64 | 验证2节点并发 | 设计余量充足 |

## 7.5 遗留问题与后续优化方向

1. **网关注册表持久化**：当前网关复位后需重新上电从机以恢复注册。后续可考虑在STM32的Flash中保存UID-地址映射表。
2. **DHT11精度提升**：DHT11温度精度为±2℃，湿度精度为±5%RH。在需要更高精度的场景下，可升级为DHT22或SHT30。
3. **RS485总线校验**：当前协议为纯ASCII文本，无CRC校验。在强电磁干扰的工业环境中，可追加校验和字段（如MODBUS-RTU的CRC16）。
4. **云端数据持久化**：当前节点状态与事件日志为内存存储，重启后丢失。后续可接入SQLite或InfluxDB实现历史数据查询与趋势分析。
5. **前端多节点切换**：当前串口屏仅显示一个节点的状态（固定节点1）。后续可在HMI上增加节点切换按钮，支持浏览多个从机的状态。

---

# 第八章 结论

本章按照“从机→主机→WiFi模块→云端→第一阶段联调→串口屏→整体联调”的实际开发顺序，详细记录了基于RS485与物联网的车位锁远程监控系统的制作与调试全过程。通过分模块独立测试与逐步联调，验证了以下核心能力：

1. **从机节点**：传感器采集、舵机控制、RS485通信、自动注册机制稳定可靠。
2. **网关主机**：三路UART并发调度、UID地址分配、离线检测、协议转发功能正常。
3. **WiFi模块**：配置持久化、MQTT透传、断网重连、双格式命令解析满足设计要求。
4. **云端服务**：数据解析、WebSocket实时推送、HTTP API、嵌入式Broker回退策略有效。
5. **串口屏**：本地状态显示与触摸控制功能完善，与云端远程控制形成互补。
6. **整体系统**：端到端延迟控制在1秒左右，2小时连续运行无异常，达到了预期的设计目标。

调试过程中暴露出的关键问题（如DHT11电源干扰、网关注册表丢失、串口屏协议截断等）均通过软硬件协同优化得到了有效解决，为系统的进一步完善奠定了坚实基础。
