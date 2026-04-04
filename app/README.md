# ESP32-S3 N16R8 Test Apps

This directory contains standalone Zephyr apps for quick board validation.

Build examples:

```powershell
pixi run west build -p always -d build_app_hello -b esp32s3_n16r8/esp32s3/procpu app/hello_world -- "-DBOARD_ROOT=D:/Projects/zephyr-ot-clean"

pixi run west build -p always -d build_app_uart -b esp32s3_n16r8/esp32s3/procpu app/uart_terminal -- "-DBOARD_ROOT=D:/Projects/zephyr-ot-clean"
```
