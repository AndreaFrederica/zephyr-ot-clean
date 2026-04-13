# STM32F103 OT Sensor Board

## 概述

这块板基于 STM32F103C8 级别的 STM32F103XB SoC，按任务拆分为传感器从机板卡定义。

默认串口控制台使用 USART3，经 PB10/PB11 接到 UART 转 RS485 模块。

## Zephyr 板名

- `stm32f103_ot_sensor`

## 主要外设映射

- `PC13`：板载用户 LED，低电平点亮
- `PB12`：DHT11 数据脚，已在 devicetree 中注册为 `dht0`
- `PB10/PB11`：USART3，连接 RS485 模块
- `PA4`：火焰传感器数字输出
- `PA5`：火焰传感器模拟输出，对应 `ADC1_IN5`
- `PA1`：三针扩展接口信号脚，同时暴露为 `TIM2_CH2` PWM

## Devicetree 约定

- `DT_ALIAS(led0)`：板载 LED
- `DT_ALIAS(dht0)`：DHT11
- `DT_ALIAS(rs485_uart)`：RS485 对应串口，对应 alias `rs485-uart`
- `DT_ALIAS(aux_pwm)`：PA1 上的 PWM 输出，对应 alias `aux-pwm`
- `/zephyr,user` 节点：
  - `flame-do-gpios`：火焰传感器数字输出
  - `aux-signal-gpios`：PA1 扩展信号
  - `io-channels = <&adc1 5>`：火焰传感器模拟量

## 构建示例

```bash
pixi run python -m west build -p always -b stm32f103_ot_sensor -d build_stm32f103_ot_sensor app/parking_lock_slave -- "-DBOARD_ROOT=D:\\Projects\\zephyr-ot-clean"
```
