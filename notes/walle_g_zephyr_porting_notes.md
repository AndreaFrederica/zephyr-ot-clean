# walle_g Zephyr 移植笔记

## 概述

本文档记录了如何为 Zephyr RTOS 添加 walle_g 修改版芯片的编译目标。

walle_g 是一个基于 **OpenTitan Earl Grey 思路** + **Efinix/SapphireSoC 风格 BSP** 的混合体 SoC，主要用于 TFLM (TensorFlow Lite Micro) 应用。

## 与原版 Efinix Sapphire 的差异

| 特性 | Sapphire (原版) | walle_g (修改版) |
|------|-----------------|------------------|
| CPU ISA | `rv32ima` | `rv32im` (无A扩展) |
| 内存布局 | `0xF9000000` | OpenTitan风格 `0x10000000`/`0x10070000` |
| XIP | 支持 | 不支持 (RAM执行) |
| DDR | 可选 | 无物理DDR (只有地址窗口) |

## 创建的文件结构

### SoC 定义

```
zephyr/soc/efinix/walle_g/
├── CMakeLists.txt      # 构建配置
├── Kconfig             # SoC 选择和特性配置
├── Kconfig.defconfig   # 默认配置值
├── Kconfig.soc         # SoC 名称定义
└── soc.yml             # SoC 元数据
```

### 设备树

```
zephyr/dts/riscv/efinix/
└── walle_g_soc.dtsi    # 完整的 SoC 设备树定义
```

### 板级支持

```
zephyr/boards/efinix/walle_g_fpga/
├── board.cmake             # 调试/烧录配置
├── board.yml               # 板元数据
├── Kconfig                 # 板级 Kconfig
├── Kconfig.walle_g_fpga    # 板选择配置
├── walle_g_fpga.dts        # 板级设备树
├── walle_g_fpga.yaml       # 板信息
└── walle_g_fpga_defconfig  # 默认编译配置
```

## 关键配置说明

### CPU/ISA 配置

来源: `walle_g/src/bsp/efinix/EfxSapphireSoc/include/soc.h`

```c
#define SYSTEM_RISCV_ISA_RV32I 1
#define SYSTEM_RISCV_ISA_EXT_M 1
#define SYSTEM_RISCV_ISA_EXT_A 0  // 无原子扩展
#define SYSTEM_RISCV_ISA_EXT_C 0  // 无压缩指令
#define SYSTEM_RISCV_ISA_EXT_F 0  // 无单精度浮点
#define SYSTEM_RISCV_ISA_EXT_D 0  // 无双精度浮点
```

设备树对应:
```dts
riscv,isa = "rv32im_zicsr_zifencei";
```

### 时钟配置

```c
#define SYSTEM_CLINT_HZ 100000000  // 100MHz
```

Kconfig 对应:
```
config SYS_CLOCK_HW_CYCLES_PER_SEC
    default 100000000
```

### 外设地址映射

| 外设 | 地址 | 大小 | 中断号 |
|------|------|------|--------|
| PLIC | 0xF8C00000 | 4MB | - |
| CLINT | 0xF8B00000 | 64KB | - |
| UART0 | 0xF8010000 | 64B | 1 |
| SPI0 | 0xF8014000 | 4KB | 2 |
| I2C0 | 0xF8015000 | 256B | 3 |
| RAM_A | 0xF9000000 | 4KB | - |

### 内存布局

来源: `walle_g/walle_sram.ld` (OpenTitan 测试 RAM 风格)

| 区域 | 起始地址 | 大小 | 用途 |
|------|----------|------|------|
| ram_code | 0x10000000 | 448KB | 代码执行区 |
| ram_main | 0x10070000 | 64KB | 数据/bss/堆/栈 |

**重要**: `SYSTEM_DDR_BMB` 只是地址窗口定义，**没有物理 DDR 连接**！

## 使用方法

```bash
# 进入 Zephyr 项目目录
cd d:/Projcets/zephyr-ot/zephyrproject

# 构建 hello_world 示例
west build -b walle_g_fpga samples/hello_world

# 清理并重新构建
west build -b walle_g_fpga samples/hello_world --pristine
```

## 后续工作

1. 验证 UART 输出
2. 启用中断 (PLIC + CLINT)
3. 添加 SPI/I2C 驱动测试
4. 集成 TFLM

## 参考文件

- 原始 SoC 定义: `walle_g/src/bsp/efinix/EfxSapphireSoc/include/soc.h`
- 链接脚本: `walle_g/walle_sram.ld`
- 研究笔记: `notes/walle_g_zephyr_fpga_study.md`
