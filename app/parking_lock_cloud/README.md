# Parking Lock Cloud MVP

## What It Does

- Receives gateway lines over MQTT `parking_lock/gateway/up/raw` (or HTTP fallback)
- Parses `REPORT/OFFLINE/ACK/ERR/BOOT`
- Maintains in-memory node states (`1..4`)
- Exposes query APIs for nodes/events
- Provides a simple cloud-to-device command queue API
- Serves a simple web UI at `/` to view and control online nodes
- Web UI uses WebSocket `/ws` for real-time broadcast (no frequent polling)

## Quick Start

1) Install dependencies:

```bash
pixi run python -m pip install fastapi uvicorn pyserial paho-mqtt amqtt
```

2) MQTT broker options:

- Option A (recommended): external broker (mosquitto)
- Option B: pure Python embedded broker (amqtt) auto-started by `server.py` when no external broker is reachable

```bash
# 示例（已安装 mosquitto 的情况下）
mosquitto -v
```

3) Run cloud server:

```bash
pixi run python -m uvicorn app.parking_lock_cloud.server:app --host 0.0.0.0 --port 8000 --reload
```

By default, server tries `127.0.0.1:1883`:

- If broker exists, it connects to external broker.
- If broker is unavailable and `amqtt` is installed, it auto starts embedded broker.

Then open:

- http://127.0.0.1:8000/

3b) Run cloud server with embedded MQTT broker via one startup script:

```bash
pixi run python app/parking_lock_cloud/run_embedded_server.py --host 0.0.0.0 --port 8000 --mqtt-host 127.0.0.1 --mqtt-port 1883 --embed-host 0.0.0.0 --reload
```

This script forces `PARKING_LOCK_EMBED_BROKER=1` and starts the backend plus the embedded `amqtt` broker together.

Or use the pixi task:

```bash
pixi run parking_lock_cloud
```

For device integration and longer-running tests, prefer the non-reload task:

```bash
pixi run parking_lock_cloud_prod
```

4) Run serial->MQTT bridge:

```bash
pixi run python app/parking_lock_cloud/serial_mqtt_bridge.py --port COM16 --baud 115200 --broker 127.0.0.1 --broker-port 1883
```

5) (Optional) Run serial->HTTP bridge (legacy fallback):

```bash
pixi run python app/parking_lock_cloud/serial_http_bridge.py --port COM16 --baud 115200 --base-url http://127.0.0.1:8000
```

## Core APIs

- `GET /health`
- `POST /v1/ingest/line`
- `POST /v1/ingest/batch`
- `GET /v1/nodes`
- `GET /v1/nodes/{node_id}`
- `GET /v1/events?limit=100`
- `POST /v1/commands`
- `GET /v1/commands/pending`
- `POST /v1/commands/{command_id}/ack`

## Example Ingest

```bash
curl -X POST http://127.0.0.1:8000/v1/ingest/line ^
  -H "Content-Type: application/json" ^
  -d "{\"line\":\"REPORT,4,POLL,T=26.00,H=55.00,FD=1,FA=3289,L=0,UID=6714285050678849\"}"
```


# 主机板（stm32f103_ot_gateway）编译
pixi run west build -b stm32f103_ot_gateway app/parking_lock_gateway -d build/parking_lock_gateway -- -DBOARD_ROOT=D:/Projects/zephyr-ot-clean

# 主机板（DAPLink）烧录
cmake -E env PYTHONPATH=D:/Projects/zephyr-ot-clean/.west_shim pixi run python -m west flash -d build/parking_lock_gateway --runner openocd --config interface/cmsis-dap.cfg --config target/stm32f1x.cfg --cmd-pre-init "adapter speed 100" --cmd-pre-init "reset_config srst_only srst_nogate connect_assert_srst"


# 从机板（stm32f103_ot_sensor）编译
pixi run west build -b stm32f103_ot_sensor app/parking_lock_slave -d build/parking_lock_slave -- -DBOARD_ROOT=D:/Projects/zephyr-ot-clean

# 从机板（DAPLink）烧录
cmake -E env PYTHONPATH=D:/Projects/zephyr-ot-clean/.west_shim pixi run python -m west flash -d build/parking_lock_slave --runner openocd --config interface/cmsis-dap.cfg --config target/stm32f1x.cfg --cmd-pre-init "adapter speed 100" --cmd-pre-init "reset_config srst_only srst_nogate connect_assert_srst"
