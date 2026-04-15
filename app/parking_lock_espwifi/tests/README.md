# ESP WiFi-MQTT 透传模块测试

本目录包含两类测试：

1. `test_espwifi_logic.py` — 纯 Python 单元测试（无需硬件）
2. `test_espwifi_serial.py` — 模拟 MCU 与 ESP 模块的串口集成测试（需要真实硬件）

---

## 1. 纯逻辑单元测试

验证从 `main/main.ino` 和 `32/32.ino` 中提取的核心协议逻辑：
- CSV / NETCFG 解析
- MQTT JSON 下行命令解析
- Topic 构建、状态映射、UART 回复生成

### 运行

```bash
pixi run python app/parking_lock_espwifi/tests/test_espwifi_logic.py -v
```

---

## 2. 串口集成测试（模拟单片机）

`test_espwifi_serial.py` 扮演 **MCU** 的角色，通过 UART 与 ESP 模块交互，并可配合 MQTT Broker 验证上下行透传。

### 硬件准备

1. ESP 模块已烧录固件并上电。
2. PC 通过 USB-TTL 连接到 ESP 模块的 UART（ESP8266 为 `Serial`；ESP32 为 `Serial2`，对应 GPIO16/17）。
3. 若需要测试 MQTT 功能，请确保本地（或指定）MQTT Broker 可访问。

### 常用命令

#### 列出可用串口
```bash
pixi run python app/parking_lock_espwifi/tests/test_espwifi_serial.py --list-ports
```

#### 仅运行 UART 测试（跳过 MQTT）
```bash
pixi run python app/parking_lock_espwifi/tests/test_espwifi_serial.py --port COM3 --no-mqtt
```

#### 运行完整测试（含 MQTT）
```bash
pixi run python app/parking_lock_espwifi/tests/test_espwifi_serial.py \
  --port COM3 \
  --mqtt-host 127.0.0.1 \
  --mqtt-port 1883 \
  --client-id test-gateway
```

### 测试项一览

| 测试名 | 说明 |
|--------|------|
| Boot Ready | 等待 `READY,ESP,1` |
| NETCFG + NETSTAT | 发送网络配置并查询状态 |
| NETCLR | 清除配置并验证返回 `EMPTY` |
| READY Gateway | `READY,GATEWAY,1` -> `INFO,ESP,gateway_ready_seen` |
| PONG Gateway | `PONG,GATEWAY` -> `INFO,ESP,pong_gateway` |
| Uplink REPORT | MCU `REPORT,...` -> MQTT `uplink/raw` |
| Uplink OFFLINE | MCU `OFFLINE,...` -> MQTT `uplink/raw` |
| Uplink ACK | MCU `ACK,...` -> MQTT `uplink/ack` |
| Downlink LOCK | MQTT `downlink/cmd` (LOCK) -> UART `CMD,...` |
| Downlink GET | MQTT `downlink/cmd` (GET) -> UART `CMD,...` |
| Downlink Bad JSON | 非法 JSON -> `ERR,ESP,mqtt_bad_json` |

---

## 注意事项

- **波特率**：默认 `115200`，与固件一致。
- **client_id**：测试程序默认使用 `test-gateway`，可通过 `--client-id` 修改。
- 若模块在测试启动前已经完成 `setup()` 并发送了 `READY,ESP,1`，`Boot Ready` 测试可能会超时。此时可重启模块后重试，或忽略该项（它不影响后续命令测试）。
