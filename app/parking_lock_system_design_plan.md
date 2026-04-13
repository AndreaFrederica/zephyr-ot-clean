# 基于 RS485 的地下车库车位锁系统设计与计划

## 1. 系统目标

- 主从分层：主节点负责汇聚、展示、上云；从节点负责采集与执行
- 现场总线：主从通过 RS485 半双工通讯
- 业务功能：
  - 从节点采集温湿度、火焰状态与火焰模拟量
  - 从节点执行车位锁控制
  - 主节点显示各从节点状态并上报云端

## 2. 总体架构

- 单片机侧
  - 从机板：`stm32f103_ot_sensor`
  - 主机板：`stm32f103_ot_gateway`
- 服务器侧
  - 运行环境：pixi
  - 后端：Python + FastAPI
  - 前端：Quasar CLI + Vue + pnpm
- 通讯链路
  - 从机 ↔ 主机：RS485（PB10/PB11 对应 USART3）
  - 主机 ↔ 串口屏：USART1（PA9/PA10）
  - 主机 ↔ ESP8266：USART2（PA2/PA3）
  - 主机 ↔ 服务器：WiFi TCP/HTTP(MQTT 可作为后续扩展)

## 3. 单片机软件设计

### 3.1 从机（parking_lock_slave）

- 功能模块
  - 传感器采集：DHT11 + 火焰数字 + 火焰模拟 ADC
  - 执行器控制：车位锁控制脚（PA1）
  - 通讯处理：RS485 命令接收与状态上报
- 协议草案
  - 主机请求：`REQ,<node_id>,PING`
  - 主机请求：`REQ,<node_id>,GET`
  - 主机控制：`SET,<node_id>,LOCK,<0|1>`
  - 从机应答：`ACK,<node_id>,PONG`
  - 从机上报：`REPORT,<node_id>,<reason>,T=..,H=..,FD=..,FA=..,L=..,DE=..,AE=..`
- 实现现状
  - 已完成 C++ 版本应用骨架与主循环
  - 已实现周期上报、命令处理、锁控制回执

### 3.2 主机（parking_lock_gateway）

- 功能模块
  - 总线轮询：按节点列表循环发送 `REQ,<id>,GET`
  - 数据分发：RS485 数据同时转发至串口屏与 WiFi 模块
  - 上行接口：通过 ESP8266 透传给服务器
  - 控制通道：串口屏/WiFi 下发 `LOCK,<id>,<0|1>` 与 `GET,<id>`
- 实现现状
  - 已完成 C++ 版本网关应用骨架
  - 已实现多节点轮询、状态维护、离线判定

## 4. 服务器与前端设计

### 4.1 后端（FastAPI）

- 目录建议
  - `server/api`：路由层（节点上报、节点控制、状态查询）
  - `server/service`：业务层（设备状态机、告警、缓存）
  - `server/store`：数据存储适配（先 SQLite，后续可切 PostgreSQL）
- 核心 API
  - `POST /api/v1/nodes/{id}/report`
  - `POST /api/v1/nodes/{id}/lock`
  - `GET /api/v1/nodes`
  - `GET /api/v1/alerts`

### 4.2 前端（Quasar）

- 页面建议
  - 总览页：在线节点数、告警数、锁状态分布
  - 节点页：温湿度曲线、火焰状态、锁控制按钮
  - 告警页：火焰告警、离线告警
- 技术建议
  - 数据请求：axios + token
  - 状态管理：pinia
  - 图表：echarts

## 5. 分阶段计划

- 阶段 A：单片机闭环
  - 从机传感器采集与锁控制稳定
  - 主机可稳定轮询多个从机并在串口屏显示
- 阶段 B：主机联网
  - ESP8266 AT 指令链路稳定
  - 主机完成上报重试和离线缓存
- 阶段 C：云端联调
  - FastAPI 接收上报、提供控制接口
  - 前端完成实时看板与控制台
- 阶段 D：可靠性优化
  - RS485 帧校验与地址化
  - 告警策略、日志追踪、异常恢复

## 6. 当前代码组织

- 外部板级定义目录：`boards/others/stm32f103_ot_sensor`、`boards/others/stm32f103_ot_gateway`
- 从机应用：`app/parking_lock_slave`
- 主机应用：`app/parking_lock_gateway`

## 7. 构建命令

```bash
pixi run python -m west build -p always -b stm32f103_ot_sensor -d build_stm32f103_ot_sensor app/parking_lock_slave -- "-DBOARD_ROOT=D:\\Projects\\zephyr-ot-clean"

pixi run python -m west build -p always -b stm32f103_ot_gateway -d build_stm32f103_ot_gateway app/parking_lock_gateway -- "-DBOARD_ROOT=D:\\Projects\\zephyr-ot-clean"
```
