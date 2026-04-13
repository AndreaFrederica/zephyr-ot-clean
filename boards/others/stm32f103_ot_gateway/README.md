# STM32F103 OT Gateway Board

## 概述

这块板基于 STM32F103C8 级别的 STM32F103XB SoC，按任务拆分为主节点网关板卡定义。

默认串口控制台使用 USART3，经 PB10/PB11 接到 UART 转 RS485 模块。

## Zephyr 板名

- `stm32f103_ot_gateway`

## 主要外设映射

- `PC13`：板载用户 LED，低电平点亮
- `PA9/PA10`：USART1，连接串口屏
- `PA2/PA3`：USART2，连接 ESP8266
- `PB10/PB11`：USART3，连接 RS485 模块

## Devicetree 约定

- `DT_ALIAS(led0)`：板载 LED
- `DT_ALIAS(screen_uart)`：串口屏对应串口，对应 alias `screen-uart`
- `DT_ALIAS(wifi_uart)`：ESP8266 对应串口，对应 alias `wifi-uart`
- `DT_ALIAS(rs485_uart)`：RS485 对应串口，对应 alias `rs485-uart`

## 构建示例

```bash
pixi run python -m west build -p always -b stm32f103_ot_gateway -d build_stm32f103_ot_gateway app/parking_lock_gateway -- "-DBOARD_ROOT=D:\\Projects\\zephyr-ot-clean"
```
