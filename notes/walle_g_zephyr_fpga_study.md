# walle_g 项目研究笔记（Zephyr 派生板卡 + FPGA 测试）

## 1. 项目定位与来源

- 当前工程是 **OpenTitan Earl Grey 思路** + **Efinix/SaxonSoc 风格 BSP/驱动** 的混合体。
- 主要应用是 TFLM（TensorFlow Lite Micro）+ signal 前处理，入口在 `src/main.cpp`。
- 启动与链接脚本有两套来源：
  - 工程主链接脚本：`walle_sram.ld`（更接近 OpenTitan RAM 启动模型）
  - BSP 示例链接脚本：`src/bsp/efinix/EfxSapphireSoc/linker/default.ld` / `default_i.ld`（Efinix 风格）

## 2. 关键内存布局（以 walle_sram.ld 为准）

来源：`walle_sram.ld`

### 2.1 MEMORY 区域

| 区域 | 起始地址 | 大小 | 属性 | 用途 |
|---|---:|---:|---|---|
| `ram_code` | `0x10000000` | `0x70000` (448 KB) | `rx` | 启动向量 + `.crt/.text/.rodata` + `.chip_info` + `.data` LMA |
| `ram_main` | `0x10070000` | `0x10000` (64 KB) | `rwx` | `.static_critical/.data/.bss/heap/stack/exception_frame` |
| `rom` | `0x00008000` | `0x8000` (32 KB) | `rx` | 只定义了窗口，当前镜像主要不在此执行 |
| `rom_ext_virtual` | `0x90000000` | `0x80000` | `rx` | 虚拟窗口 |
| `owner_virtual` | `0xA0000000` | `0x80000` | `rx` | 虚拟窗口 |

### 2.2 段布局特性

- 启动地址：`_boot_address = ORIGIN(ram_code)`，即 `0x10000000`
- 向量表：`.vectors` 固定放在 `_boot_address`，256-byte 对齐（见 `src/ram_start.S`）
- 栈和异常帧在 `ram_main` 顶部：
  - `_exception_frame_size = 200`
  - `_stack_size = 16384 - _exception_frame_size`
- `.data` 采用 `VMA=ram_main, LMA=ram_code`（CRT 复制到 RAM）
- `__heap_start` 在 `.bss` 之后对齐到 8-byte

### 2.3 Zephyr 迁移风险点（内存）

1. 代码在 SRAM 执行（非典型 Flash XIP）
2. `ram_main` 仅 64KB，需严控 `CONFIG_MAIN_STACK_SIZE/HEAP/MEM_POOL`
3. `.static_critical` 区域有安全/状态语义，若迁移要决定是否保留
4. 现有链接脚本包含大量 OpenTitan 专用 section（logs/status_create_record）

## 3. 启动流程（当前工程）

来源：`src/ram_start.S`

1. `_test_ram_interrupt_vector`：32 个向量入口都跳到 `_test_ram_irq_handler`
2. 向量后直接跳 `_reset_start`
3. `_reset_start`：
   - 设置 `gp`
   - 清零通用寄存器
   - 设置 `sp = _stack_end`
4. `_start`：
   - 准备 `.bss/.data` 地址（当前示例里 copy/zero 调用注释）
   - 清零临时寄存器
   - `call main`
   - 失败后 `wfi` 死循环

> 说明：这套启动更像“最小裸机入口”，而不是完整 C runtime。

## 4. SoC/外设地址映射（最重要）

来源：`src/bsp/efinix/EfxSapphireSoc/include/soc.h`

### 4.1 CPU/ISA

- RV32I + M + Zicsr + Zifence
- `A/C/F/D` 关闭（`SYSTEM_RISCV_ISA_EXT_A/C/F/D = 0`）
- `SYSTEM_CLINT_HZ = 100000000`

### 4.2 中断编号（PLIC）

- `SYSTEM_PLIC_SYSTEM_UART_0_IO_INTERRUPT = 1`
- `SYSTEM_PLIC_SYSTEM_SPI_0_IO_INTERRUPT = 2`
- `SYSTEM_PLIC_SYSTEM_I2C_0_IO_INTERRUPT = 3`
- `SYSTEM_PLIC_USER_INTERRUPT_A_INTERRUPT = 6`
- `SYSTEM_PLIC_USER_INTERRUPT_B_INTERRUPT = 7`
- `SYSTEM_PLIC_SYSTEM_AXI_A_INTERRUPT = 30`

### 4.3 基地址（Zephyr dts 首批要覆盖）

- `SYSTEM_PLIC_CTRL = 0xF8C00000`（size `0x400000`）
- `SYSTEM_CLINT_CTRL = 0xF8B00000`（size `0x10000`）
- `SYSTEM_UART_0_IO_CTRL = 0xF8010000`（size `0x40`）
- `SYSTEM_SPI_0_IO_CTRL = 0xF8014000`（size `0x1000`）
- `SYSTEM_I2C_0_IO_CTRL = 0xF8015000`（size `0x100`）
- `IO_APB_SLAVE_0_INPUT = 0xF8100000`
- `IO_APB_SLAVE_1_INPUT = 0xF8110000`
- `SYSTEM_RAM_A_CTRL = 0xF9000000`（size `0x1000`）

### 4.4 当前中断驱动状态

来源：`src/platform/interrupt/intc.c`

- `trap()` 中 `mcause` 读取目前被写死为 `0`，CSR 读取被注释，意味着异常/中断路径实际上未完整启用。
- `IntcInitialize()` 里 `mtvec/mie/mstatus` 的 CSR 配置同样被注释。
- 当前只显式处理了 `SYSTEM_PLIC_USER_INTERRUPT_A_INTERRUPT`，用于 `ops_drv_intr()`。

> 对 FPGA 验证很关键：若想做 Zephyr 中断联调，先要恢复真实 trap 初始化路径。

## 5. 驱动层风格与 Zephyr 适配建议

来源：`src/driver/*.h`

- 驱动是 header-inline + MMIO 直接访问模式（`io.h` 提供 `read_u32/write_u32`）
- 已有模块：`uart/gpio/spi/i2c/plic/clint/timer/...`
- 寄存器偏移定义较完整，适合做 Zephyr driver shim

建议分层：

1. **板级 dts 描述**：先把地址与中断号精确化
2. **最小可启动集**：`uart + plic + clint + gpio`
3. **再接入功能外设**：`spi/i2c`，最后接 TinyML 相关中断路径

## 6. Zephyr 派生板卡落地清单（建议顺序）

1. 新建 board 目录（riscv32）：
   - `boards/riscv/<board_name>/<board_name>.dts`
   - `boards/riscv/<board_name>/<board_name>_defconfig`
   - `boards/riscv/<board_name>/board.cmake`
2. dts 首版先放：CPU、SRAM、PLIC、CLINT、UART
3. 关闭不匹配 ISA 特性：确保无 RV32C/A/F/D 依赖
4. 确认时钟：`SYS_CLOCK_HW_CYCLES_PER_SEC = 100000000`
5. 先跑 `hello_world + uart`，再开中断测试
6. 再加入 `spi/i2c` 节点并做 loopback/外设连测

## 7. FPGA 实测建议（快速排障）

### 7.1 上电首测

- 串口是否有早期输出（若无，先查 UART 时钟分频）
- `mtvec` 是否正确设置到 trap 入口
- PLIC claim/release 是否能收到真实外设中断号

### 7.2 最小功能回归

1. UART TX/RX
2. CLINT 计时（busy wait）
3. PLIC + 1 个 GPIO/用户中断
4. SPI 回环
5. I2C 基本读写

### 7.3 常见坑

- 链接地址和实际加载地址不一致（尤其 SRAM 执行场景）
- trap 初始化没开，导致“外设工作但中断不进”
- Zephyr dts 中断 parent/irq 编号与 `soc.h` 不一致

## 8. 当前结论（给你后续建板用）

- 这不是“纯 OpenTitan Earl Grey 原版”，而是混合并修改过外设地址/中断/驱动风格的 SoC 变体。
- 创建 Zephyr 派生板卡时，**必须以 `soc.h + walle_sram.ld + ram_start.S` 的交集为真值**，不要直接套 OpenTitan upstream 的内存和外设定义。
- 你要在 FPGA 上测，优先把 `UART + PLIC + CLINT + trap` 跑通，再逐步接入其它外设和 TinyML 中断链路。

## 9. 关键地址原始来源（可点击）

### 9.1 内存地址（来自链接脚本）

- `ram_main ORIGIN = 0x10070000`： [walle_sram.ld](../walle_sram.ld#L28)
- `ram_code ORIGIN = 0x10000000`： [walle_sram.ld](../walle_sram.ld#L29)
- `_boot_address = ORIGIN(ram_code)`： [walle_sram.ld](../walle_sram.ld#L79)

### 9.2 外设基地址（来自 soc.h）

- `SYSTEM_PLIC_CTRL = 0xF8C00000`： [src/bsp/efinix/EfxSapphireSoc/include/soc.h](../src/bsp/efinix/EfxSapphireSoc/include/soc.h#L51)
- `SYSTEM_CLINT_CTRL = 0xF8B00000`： [src/bsp/efinix/EfxSapphireSoc/include/soc.h](../src/bsp/efinix/EfxSapphireSoc/include/soc.h#L53)
- `SYSTEM_UART_0_IO_CTRL = 0xF8010000`： [src/bsp/efinix/EfxSapphireSoc/include/soc.h](../src/bsp/efinix/EfxSapphireSoc/include/soc.h#L55)
- `SYSTEM_SPI_0_IO_CTRL = 0xF8014000`： [src/bsp/efinix/EfxSapphireSoc/include/soc.h](../src/bsp/efinix/EfxSapphireSoc/include/soc.h#L57)
- `SYSTEM_I2C_0_IO_CTRL = 0xF8015000`： [src/bsp/efinix/EfxSapphireSoc/include/soc.h](../src/bsp/efinix/EfxSapphireSoc/include/soc.h#L59)

### 9.3 中断号（来自 soc.h）

- `SYSTEM_PLIC_USER_INTERRUPT_A_INTERRUPT = 6`： [src/bsp/efinix/EfxSapphireSoc/include/soc.h](../src/bsp/efinix/EfxSapphireSoc/include/soc.h#L4)
- `SYSTEM_PLIC_SYSTEM_UART_0_IO_INTERRUPT = 1`： [src/bsp/efinix/EfxSapphireSoc/include/soc.h](../src/bsp/efinix/EfxSapphireSoc/include/soc.h#L7)