import argparse
import json
import threading
import time

import paho.mqtt.client as mqtt
import serial


def to_serial_line(action: str, node_id: int, value: int | None) -> str | None:
    action = action.upper()
    if action == "LOCK" and value in {0, 1}:
        return f"LOCK,{node_id},{value}"
    if action == "GET":
        return f"GET,{node_id}"
    return None


def parse_command_payload(payload: str) -> tuple[str | None, str | None]:
    payload = payload.strip()
    if not payload:
        return None, None

    if payload.startswith("CMD,"):
        parts = payload.split(",")
        if len(parts) == 5 and parts[2].upper() == "LOCK" and parts[4] in {"0", "1"}:
            return parts[1], f"LOCK,{parts[3]},{parts[4]}"
        if len(parts) == 4 and parts[2].upper() == "GET":
            return parts[1], f"GET,{parts[3]}"
        return (parts[1] if len(parts) > 1 else None), None

    try:
        data = json.loads(payload)
        cmd_id = str(data["command_id"])
        node_id = int(data["node_id"])
        action = str(data["action"])
        value = data.get("value")
    except Exception:
        return None, None

    return cmd_id, to_serial_line(action, node_id, value)


class Bridge:
    def __init__(
        self,
        ser: serial.Serial,
        mqtt_client: mqtt.Client,
        topic_up_raw: str,
        topic_cmd: str,
        topic_cmd_ack: str,
    ) -> None:
        self.ser = ser
        self.mqtt = mqtt_client
        self.topic_up_raw = topic_up_raw
        self.topic_cmd = topic_cmd
        self.topic_cmd_ack = topic_cmd_ack
        self.stop_event = threading.Event()

    def reader_loop(self) -> None:
        while not self.stop_event.is_set():
            try:
                line = self.ser.readline()
            except serial.SerialException:
                break

            if not line:
                continue

            text = line.decode(errors="ignore").strip()
            if not text:
                continue

            print(f"[SER RX] {text}")
            self.mqtt.publish(self.topic_up_raw, payload=text, qos=0, retain=False)

    def on_mqtt_message(self, client: mqtt.Client, userdata: object, msg: mqtt.MQTTMessage) -> None:
        payload = msg.payload.decode(errors="ignore").strip()
        print(f"[MQTT RX] {payload}")
        cmd_id, line = parse_command_payload(payload)
        if cmd_id is None and line is None:
            print("[WARN] bad command payload")
            return

        if line is None:
            self._publish_ack(cmd_id, "failed", "bad_command")
            return

        try:
            self.ser.write((line + "\r\n").encode())
            self.ser.flush()
        except Exception as exc:
            self._publish_ack(cmd_id, "failed", f"serial_error:{exc}")
            return

        print(f"[SER TX] {line}")
        self._publish_ack(cmd_id, "sent", "serial_written")

    def _publish_ack(self, command_id: str | None, status: str, detail: str) -> None:
        if not command_id:
            return
        payload = f"ACK,{command_id},{status},{detail}"
        self.mqtt.publish(self.topic_cmd_ack, payload=payload, qos=1, retain=False)


def main() -> None:
    parser = argparse.ArgumentParser(description="Serial <-> MQTT bridge for parking lock")
    parser.add_argument("--port", default="COM16")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--serial-timeout", type=float, default=0.2)
    parser.add_argument("--broker", default="127.0.0.1")
    parser.add_argument("--broker-port", type=int, default=1883)
    parser.add_argument("--topic-up-raw", default="parking_lock/gateway/up/raw")
    parser.add_argument("--topic-cmd", default="parking_lock/cloud/command")
    parser.add_argument("--topic-cmd-ack", default="parking_lock/gateway/command_ack")
    args = parser.parse_args()

    with serial.Serial(args.port, args.baud, timeout=args.serial_timeout) as ser:
        print(f"[INFO] Serial connected: {args.port} @ {args.baud}")

        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1)
        bridge = Bridge(ser, client, args.topic_up_raw, args.topic_cmd, args.topic_cmd_ack)

        def _on_connect(c: mqtt.Client, userdata: object, flags: object, rc: int) -> None:
            if rc == 0:
                print("[INFO] MQTT connected")
                c.subscribe(args.topic_cmd)
            else:
                print(f"[ERR] MQTT connect failed: rc={rc}")

        def _on_disconnect(c: mqtt.Client, userdata: object, rc: int) -> None:
            print(f"[WARN] MQTT disconnected rc={rc}, waiting reconnect...")

        client.on_connect = _on_connect
        client.on_disconnect = _on_disconnect
        client.on_message = bridge.on_mqtt_message
        client.reconnect_delay_set(min_delay=1, max_delay=8)
        client.connect_async(args.broker, args.broker_port, keepalive=60)
        client.loop_start()

        t = threading.Thread(target=bridge.reader_loop, daemon=True)
        t.start()
        try:
            while True:
                time.sleep(0.5)
        except KeyboardInterrupt:
            pass
        finally:
            bridge.stop_event.set()
            t.join(timeout=0.5)
            client.loop_stop()
            client.disconnect()


if __name__ == "__main__":
    main()
