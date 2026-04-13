# Zephyr Apps

本目录包含项目相关 Zephyr 应用。

## 车位锁系统应用

- 从机应用：`app/parking_lock_slave`
- 主机应用：`app/parking_lock_gateway`
- 总体设计文档：`app/parking_lock_system_design_plan.md`

构建示例：

```powershell
pixi run python -m west build -p always -d build_stm32f103_ot_sensor -b stm32f103_ot_sensor app/parking_lock_slave -- "-DBOARD_ROOT=D:/Projects/zephyr-ot-clean"

pixi run python -m west build -p always -d build_stm32f103_ot_gateway -b stm32f103_ot_gateway app/parking_lock_gateway -- "-DBOARD_ROOT=D:/Projects/zephyr-ot-clean"
```

## 其他示例应用

- `app/hello_world`
- `app/uart_terminal`
