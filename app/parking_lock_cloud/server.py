from __future__ import annotations

from collections import deque
from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
import asyncio
import json
import os
from pathlib import Path
import socket
from typing import Any
from uuid import uuid4

from fastapi import FastAPI, HTTPException, Query, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
import paho.mqtt.client as mqtt
from pydantic import BaseModel, Field

try:
    from amqtt.broker import Broker as AmqttBroker
except Exception:
    AmqttBroker = None


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def parse_int(text: str) -> int | None:
    try:
        return int(text)
    except Exception:
        return None


def parse_kv_fields(parts: list[str], start_idx: int) -> dict[str, str]:
    out: dict[str, str] = {}
    for item in parts[start_idx:]:
        if "=" not in item:
            continue
        key, value = item.split("=", 1)
        out[key] = value
    return out


@dataclass
class NodeState:
    node_id: int
    online: bool = False
    temp: str = "-"
    humi: str = "-"
    flame_digital: str = "-"
    flame_analog: str = "-"
    lock_state: str = "-"
    uid: str = "-"
    reason: str = "-"
    last_seen: str = "-"
    last_raw: str = "-"
    updated_at: str = field(default_factory=utc_now)


@dataclass
class EventItem:
    ts: str
    level: str
    event: str
    raw: str
    extra: dict[str, Any]


@dataclass
class CommandItem:
    id: str
    created_at: str
    node_id: int
    action: str
    value: int | None
    status: str
    ack_at: str | None


class IngestLineRequest(BaseModel):
    line: str = Field(..., description="Raw line from gateway, e.g. REPORT,...")


class IngestBatchRequest(BaseModel):
    lines: list[str]


class CommandCreateRequest(BaseModel):
    node_id: int = Field(..., ge=1, le=4)
    action: str = Field(..., description="LOCK or GET")
    value: int | None = Field(None, description="For LOCK only: 0 or 1")


class CommandAckRequest(BaseModel):
    status: str = Field(..., description="done or failed")


app = FastAPI(title="Parking Lock Cloud MVP", version="0.1.0")
BASE_DIR = Path(__file__).resolve().parent
STATIC_DIR = BASE_DIR / "static"
app.mount("/static", StaticFiles(directory=str(STATIC_DIR)), name="static")

NODES: dict[int, NodeState] = {i: NodeState(node_id=i) for i in range(1, 5)}
EVENTS: deque[EventItem] = deque(maxlen=1500)
COMMANDS: dict[str, CommandItem] = {}

MQTT_BROKER = os.getenv("PARKING_LOCK_MQTT_BROKER", "127.0.0.1")
MQTT_PORT = int(os.getenv("PARKING_LOCK_MQTT_PORT", "1883"))
TOPIC_UP_RAW = os.getenv("PARKING_LOCK_TOPIC_UP_RAW", "parking_lock/gateway/up/raw")
TOPIC_CMD = os.getenv("PARKING_LOCK_TOPIC_CMD", "parking_lock/cloud/command")
TOPIC_CMD_ACK = os.getenv("PARKING_LOCK_TOPIC_CMD_ACK", "parking_lock/gateway/command_ack")
EMBED_BROKER_ENABLED = os.getenv("PARKING_LOCK_EMBED_BROKER", "1") not in {"0", "false", "False"}
EMBED_BROKER_HOST = os.getenv("PARKING_LOCK_EMBED_HOST", MQTT_BROKER)
EMBED_BROKER_PORT = int(os.getenv("PARKING_LOCK_EMBED_PORT", str(MQTT_PORT)))

mqtt_client: mqtt.Client | None = None
mqtt_connected = False
embedded_broker: Any = None
embedded_broker_started = False
app_loop: asyncio.AbstractEventLoop | None = None


class WsManager:
    def __init__(self) -> None:
        self.clients: set[WebSocket] = set()
        self._lock = asyncio.Lock()

    async def connect(self, websocket: WebSocket) -> None:
        await websocket.accept()
        async with self._lock:
            self.clients.add(websocket)

    async def disconnect(self, websocket: WebSocket) -> None:
        async with self._lock:
            if websocket in self.clients:
                self.clients.remove(websocket)

    async def broadcast_json(self, payload: dict[str, Any]) -> None:
        async with self._lock:
            targets = list(self.clients)

        dropped: list[WebSocket] = []
        for ws in targets:
            try:
                await ws.send_json(payload)
            except Exception:
                dropped.append(ws)

        if dropped:
            async with self._lock:
                for ws in dropped:
                    self.clients.discard(ws)


ws_manager = WsManager()


def push_event(level: str, event: str, raw: str, extra: dict[str, Any] | None = None) -> None:
    EVENTS.append(
        EventItem(
            ts=utc_now(),
            level=level,
            event=event,
            raw=raw,
            extra=extra or {},
        )
    )


def on_mqtt_connect(client: mqtt.Client, userdata: Any, flags: Any, rc: int) -> None:
    global mqtt_connected
    mqtt_connected = rc == 0
    if mqtt_connected:
        client.subscribe(TOPIC_UP_RAW)
        client.subscribe(TOPIC_CMD_ACK)
        push_event("info", "mqtt_connected", f"{MQTT_BROKER}:{MQTT_PORT}", {})
    else:
        push_event("error", "mqtt_connect_failed", f"rc={rc}", {})


def on_mqtt_message(client: mqtt.Client, userdata: Any, msg: mqtt.MQTTMessage) -> None:
    payload = msg.payload.decode(errors="ignore").strip()
    if msg.topic == TOPIC_UP_RAW:
        handle_ingest_line(payload)
        push_snapshot_from_thread()
        return


def can_connect_tcp(host: str, port: int, timeout_s: float = 0.6) -> bool:
    try:
        with socket.create_connection((host, port), timeout=timeout_s):
            return True
    except Exception:
        return False

    if msg.topic == TOPIC_CMD_ACK:
        try:
            data = json.loads(payload)
        except Exception:
            push_event("warn", "mqtt_ack_bad_json", payload, {})
            return
        cmd_id = data.get("command_id")
        status = str(data.get("status", "")).lower()
        if not cmd_id or cmd_id not in COMMANDS:
            push_event("warn", "mqtt_ack_unknown_cmd", payload, {})
            return
        item = COMMANDS[cmd_id]
        if status in {"sent", "done", "failed"}:
            item.status = status
            if status in {"done", "failed"}:
                item.ack_at = utc_now()
            push_event("info", "mqtt_command_ack", payload, {"command_id": cmd_id})
            push_snapshot_from_thread()
        return


def make_snapshot(limit_events: int = 80) -> dict[str, Any]:
    return {
        "type": "snapshot",
        "nodes": [asdict(NODES[i]) for i in sorted(NODES.keys())],
        "events": [asdict(ev) for ev in list(EVENTS)[-limit_events:]],
        "mqtt_connected": mqtt_connected,
        "embedded_broker_started": embedded_broker_started,
        "time": utc_now(),
    }


def push_snapshot_from_thread() -> None:
    if app_loop is None:
        return
    fut = asyncio.run_coroutine_threadsafe(ws_manager.broadcast_json(make_snapshot()), app_loop)
    try:
        fut.result(timeout=0)
    except Exception:
        pass


def handle_ingest_line(raw_line: str) -> dict[str, Any]:
    line = raw_line.strip()
    if not line:
        return {"accepted": False, "reason": "empty"}

    parts = line.split(",")
    prefix = parts[0]

    if prefix == "REPORT" and len(parts) >= 4:
        node_id = parse_int(parts[1])
        if node_id is None or node_id not in NODES:
            push_event("warn", "report_bad_id", line, {})
            return {"accepted": False, "reason": "bad_node_id"}

        kv = parse_kv_fields(parts, 3)
        state = NODES[node_id]
        state.online = True
        state.temp = kv.get("T", state.temp)
        state.humi = kv.get("H", state.humi)
        state.flame_digital = kv.get("FD", state.flame_digital)
        state.flame_analog = kv.get("FA", state.flame_analog)
        state.lock_state = kv.get("L", state.lock_state)
        state.uid = kv.get("UID", state.uid)
        state.reason = parts[2]
        state.last_seen = utc_now()
        state.last_raw = line
        state.updated_at = utc_now()
        push_event("info", "report", line, {"node_id": node_id})
        return {"accepted": True, "event": "report", "node_id": node_id}

    if prefix == "OFFLINE" and len(parts) >= 2:
        node_id = parse_int(parts[1])
        if node_id is not None and node_id in NODES:
            state = NODES[node_id]
            state.online = False
            state.updated_at = utc_now()
            state.last_raw = line
            push_event("warn", "offline", line, {"node_id": node_id})
            return {"accepted": True, "event": "offline", "node_id": node_id}
        push_event("warn", "offline_bad_id", line, {})
        return {"accepted": False, "reason": "bad_node_id"}

    if prefix in {"ACK", "ERR", "BOOT", "GATEWAY_READY"}:
        push_event("info", prefix.lower(), line, {})
        return {"accepted": True, "event": prefix.lower()}

    push_event("warn", "unknown_line", line, {})
    return {"accepted": False, "reason": "unknown_prefix"}


def start_mqtt() -> None:
    global mqtt_client
    mqtt_client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1)
    mqtt_client.on_connect = on_mqtt_connect
    mqtt_client.on_message = on_mqtt_message
    mqtt_client.reconnect_delay_set(min_delay=1, max_delay=8)
    mqtt_client.connect_async(MQTT_BROKER, MQTT_PORT, keepalive=60)
    mqtt_client.loop_start()


def stop_mqtt() -> None:
    global mqtt_client, mqtt_connected
    mqtt_connected = False
    if mqtt_client is not None:
        mqtt_client.loop_stop()
        mqtt_client.disconnect()
        mqtt_client = None


async def start_embedded_broker_if_needed() -> None:
    global embedded_broker, embedded_broker_started
    if not EMBED_BROKER_ENABLED:
        push_event("info", "embedded_broker_disabled", "", {})
        return

    if can_connect_tcp(MQTT_BROKER, MQTT_PORT):
        push_event("info", "external_broker_detected", f"{MQTT_BROKER}:{MQTT_PORT}", {})
        return

    if AmqttBroker is None:
        push_event("error", "embedded_broker_unavailable", "please install amqtt", {})
        return

    broker_config = {
        "listeners": {
            "default": {
                "type": "tcp",
                "bind": f"{EMBED_BROKER_HOST}:{EMBED_BROKER_PORT}",
            }
        },
        "sys_interval": 0,
        "topic-check": {"enabled": False},
    }
    embedded_broker = AmqttBroker(broker_config)
    await embedded_broker.start()
    embedded_broker_started = True
    push_event("info", "embedded_broker_started", f"{EMBED_BROKER_HOST}:{EMBED_BROKER_PORT}", {})


async def stop_embedded_broker() -> None:
    global embedded_broker, embedded_broker_started
    if embedded_broker is not None:
        try:
            await embedded_broker.shutdown()
        finally:
            embedded_broker = None
            embedded_broker_started = False


def publish_command(command: CommandItem) -> bool:
    if mqtt_client is None or not mqtt_connected:
        return False
    payload = json.dumps(
        {
            "command_id": command.id,
            "node_id": command.node_id,
            "action": command.action,
            "value": command.value,
            "created_at": command.created_at,
        },
        ensure_ascii=False,
    )
    info = mqtt_client.publish(TOPIC_CMD, payload=payload, qos=1, retain=False)
    return info.rc == mqtt.MQTT_ERR_SUCCESS


@app.get("/health")
def health() -> dict[str, Any]:
    return {
        "ok": True,
        "time": utc_now(),
        "mqtt_connected": mqtt_connected,
        "embedded_broker_started": embedded_broker_started,
        "mqtt_target": f"{MQTT_BROKER}:{MQTT_PORT}",
    }


@app.get("/")
def index() -> FileResponse:
    return FileResponse(STATIC_DIR / "index.html")


@app.websocket("/ws")
async def ws_endpoint(websocket: WebSocket) -> None:
    await ws_manager.connect(websocket)
    await websocket.send_json(make_snapshot())
    try:
        while True:
            await websocket.receive_text()
    except WebSocketDisconnect:
        await ws_manager.disconnect(websocket)
    except Exception:
        await ws_manager.disconnect(websocket)


@app.on_event("startup")
async def on_startup() -> None:
    global app_loop
    app_loop = asyncio.get_running_loop()
    await start_embedded_broker_if_needed()
    start_mqtt()


@app.on_event("shutdown")
async def on_shutdown() -> None:
    global app_loop
    stop_mqtt()
    await stop_embedded_broker()
    app_loop = None


@app.post("/v1/ingest/line")
async def ingest_line(req: IngestLineRequest) -> dict[str, Any]:
    result = handle_ingest_line(req.line)
    await ws_manager.broadcast_json(make_snapshot())
    return result


@app.post("/v1/ingest/batch")
async def ingest_batch(req: IngestBatchRequest) -> dict[str, Any]:
    results = [handle_ingest_line(line) for line in req.lines]
    await ws_manager.broadcast_json(make_snapshot())
    return {"count": len(results), "results": results}


@app.get("/v1/nodes")
def list_nodes() -> list[dict[str, Any]]:
    return [asdict(NODES[i]) for i in sorted(NODES.keys())]


@app.get("/v1/nodes/{node_id}")
def get_node(node_id: int) -> dict[str, Any]:
    if node_id not in NODES:
        raise HTTPException(status_code=404, detail="node not found")
    return asdict(NODES[node_id])


@app.get("/v1/events")
def list_events(limit: int = Query(100, ge=1, le=500)) -> list[dict[str, Any]]:
    items = list(EVENTS)[-limit:]
    return [asdict(it) for it in items]


@app.post("/v1/commands")
async def create_command(req: CommandCreateRequest) -> dict[str, Any]:
    action = req.action.upper()
    if action not in {"LOCK", "GET"}:
        raise HTTPException(status_code=400, detail="action must be LOCK or GET")
    if action == "LOCK" and req.value not in {0, 1}:
        raise HTTPException(status_code=400, detail="LOCK requires value 0 or 1")
    if action == "GET" and req.value is not None:
        raise HTTPException(status_code=400, detail="GET should not provide value")

    cmd_id = str(uuid4())
    item = CommandItem(
        id=cmd_id,
        created_at=utc_now(),
        node_id=req.node_id,
        action=action,
        value=req.value,
        status="pending",
        ack_at=None,
    )
    COMMANDS[cmd_id] = item
    if publish_command(item):
        item.status = "sent"
    push_event(
        "info",
        "command_created",
        f"{action},{req.node_id}",
        {"command_id": cmd_id, "status": item.status},
    )
    await ws_manager.broadcast_json(make_snapshot())
    return asdict(item)


@app.get("/v1/commands/pending")
def list_pending_commands(limit: int = Query(50, ge=1, le=200)) -> list[dict[str, Any]]:
    pending = [cmd for cmd in COMMANDS.values() if cmd.status == "pending"]
    pending.sort(key=lambda x: x.created_at)
    return [asdict(cmd) for cmd in pending[:limit]]


@app.post("/v1/commands/{command_id}/ack")
async def ack_command(command_id: str, req: CommandAckRequest) -> dict[str, Any]:
    item = COMMANDS.get(command_id)
    if item is None:
        raise HTTPException(status_code=404, detail="command not found")
    status = req.status.lower()
    if status not in {"done", "failed"}:
        raise HTTPException(status_code=400, detail="status must be done or failed")
    item.status = status
    item.ack_at = utc_now()
    push_event("info", "command_ack", item.id, {"status": item.status})
    await ws_manager.broadcast_json(make_snapshot())
    return asdict(item)
